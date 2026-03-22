#include "chess_app/app.h"

#include "chess_app/game_state.h"

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/render_board.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/select.h>
#include <stdint.h>
#include <string.h>

static uint32_t make_game_id(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer)
{
    uint32_t hash = 2166136261u;
    const char *uuids[2] = { NULL, NULL };
    int i = 0;
    int j = 0;

    if (!local_peer || !remote_peer) {
        return 0u;
    }

    uuids[0] = (SDL_strncmp(local_peer->uuid, remote_peer->uuid, CHESS_UUID_STRING_LEN) <= 0)
        ? local_peer->uuid
        : remote_peer->uuid;
    uuids[1] = (uuids[0] == local_peer->uuid) ? remote_peer->uuid : local_peer->uuid;

    for (i = 0; i < 2; ++i) {
        for (j = 0; uuids[i][j] != '\0'; ++j) {
            hash ^= (uint8_t)uuids[i][j];
            hash *= 16777619u;
        }
    }

    return hash;
}

static const char *network_state_to_string(ChessNetworkState state)
{
    switch (state) {
    case CHESS_NET_IDLE_DISCOVERY:
        return "IDLE_DISCOVERY";
    case CHESS_NET_PEER_FOUND:
        return "PEER_FOUND";
    case CHESS_NET_ELECTION:
        return "ELECTION";
    case CHESS_NET_CONNECTING:
        return "CONNECTING";
    case CHESS_NET_IN_GAME:
        return "IN_GAME";
    case CHESS_NET_RECONNECTING:
        return "RECONNECTING";
    case CHESS_NET_TERMINATED:
        return "TERMINATED";
    default:
        return "UNKNOWN";
    }
}

/* ---------- chess piece glyph textures (optional, falls back to rectangles) ---------- */

static TTF_Font    *s_chess_font                     = NULL;
static SDL_Texture *s_piece_textures[CHESS_PIECE_COUNT];

/* Renders a chess glyph with a contrasting outline for readability on any square. */
static SDL_Texture *make_outlined_glyph_texture(
    SDL_Renderer *renderer,
    TTF_Font     *font,
    Uint32        codepoint,
    SDL_Color     fg,
    SDL_Color     outline_col,
    int           outline_px)
{
    SDL_Surface *outline_surf = NULL;
    SDL_Surface *fill_surf    = NULL;
    SDL_Surface *combined     = NULL;
    SDL_Texture *tex          = NULL;
    SDL_Rect     fill_dst;

    TTF_SetFontOutline(font, outline_px);
    outline_surf = TTF_RenderGlyph_Blended(font, codepoint, outline_col);
    TTF_SetFontOutline(font, 0);
    fill_surf    = TTF_RenderGlyph_Blended(font, codepoint, fg);

    if (!outline_surf || !fill_surf) {
        goto cleanup;
    }

    combined = SDL_CreateSurface(outline_surf->w, outline_surf->h, SDL_PIXELFORMAT_ARGB8888);
    if (!combined) {
        goto cleanup;
    }
    SDL_FillSurfaceRect(combined, NULL, 0u);

    SDL_SetSurfaceBlendMode(outline_surf, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(outline_surf, NULL, combined, NULL);

    fill_dst.x = outline_px;
    fill_dst.y = outline_px;
    fill_dst.w = fill_surf->w;
    fill_dst.h = fill_surf->h;
    SDL_SetSurfaceBlendMode(fill_surf, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(fill_surf, NULL, combined, &fill_dst);

    tex = SDL_CreateTextureFromSurface(renderer, combined);
    SDL_DestroySurface(combined);
    combined = NULL;

cleanup:
    if (outline_surf) SDL_DestroySurface(outline_surf);
    if (fill_surf)    SDL_DestroySurface(fill_surf);
    if (combined)     SDL_DestroySurface(combined);
    return tex;
}

static void init_piece_textures(SDL_Renderer *renderer)
{
    /* Ordered list of fonts known to carry chess glyphs (U+2654-265F) on macOS */
    static const char * const font_paths[] = {
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
        NULL
    };
    /* Codepoints indexed by ChessPiece enum value (0 = EMPTY, skipped) */
    static const Uint32 piece_codepoints[CHESS_PIECE_COUNT] = {
        [CHESS_PIECE_WHITE_PAWN]   = 0x2659u, /* ♙ */
        [CHESS_PIECE_WHITE_KNIGHT] = 0x2658u, /* ♘ */
        [CHESS_PIECE_WHITE_BISHOP] = 0x2657u, /* ♗ */
        [CHESS_PIECE_WHITE_ROOK]   = 0x2656u, /* ♖ */
        [CHESS_PIECE_WHITE_QUEEN]  = 0x2655u, /* ♕ */
        [CHESS_PIECE_WHITE_KING]   = 0x2654u, /* ♔ */
        [CHESS_PIECE_BLACK_PAWN]   = 0x265Fu, /* ♟ */
        [CHESS_PIECE_BLACK_KNIGHT] = 0x265Eu, /* ♞ */
        [CHESS_PIECE_BLACK_BISHOP] = 0x265Du, /* ♝ */
        [CHESS_PIECE_BLACK_ROOK]   = 0x265Cu, /* ♜ */
        [CHESS_PIECE_BLACK_QUEEN]  = 0x265Bu, /* ♛ */
        [CHESS_PIECE_BLACK_KING]   = 0x265Au, /* ♚ */
    };
    /* White pieces: creamy fill + dark-brown outline (readable on both square colours)
     * Black pieces: dark-brown fill + warm-cream outline */
    const SDL_Color white_fg      = {245, 238, 200, 255};
    const SDL_Color white_outline = { 50,  35,  20, 255};
    const SDL_Color black_fg      = { 45,  30,  20, 255};
    const SDL_Color black_outline = {215, 210, 175, 255};
    int i;
    float font_size = 52.0f;

    if (!TTF_Init()) {
        SDL_Log("TTF_Init failed: %s", SDL_GetError());
        return;
    }

    for (i = 0; font_paths[i] != NULL; ++i) {
        s_chess_font = TTF_OpenFont(font_paths[i], font_size);
        if (s_chess_font) {
            SDL_Log("Loaded chess font: %s", font_paths[i]);
            break;
        }
    }
    if (!s_chess_font) {
        SDL_Log("No chess font found, piece rendering will use fallback rectangles");
        TTF_Quit();
        return;
    }

    for (i = 1; i < CHESS_PIECE_COUNT; ++i) {
        bool is_white = (i < (int)CHESS_PIECE_BLACK_PAWN);
        SDL_Color fg      = is_white ? white_fg      : black_fg;
        SDL_Color outline = is_white ? white_outline  : black_outline;
        s_piece_textures[i] = make_outlined_glyph_texture(
            renderer, s_chess_font, piece_codepoints[i], fg, outline, 2);
        if (!s_piece_textures[i]) {
            SDL_Log("Failed to create texture for piece %d: %s", i, SDL_GetError());
        }
    }
}

static void destroy_piece_textures(void)
{
    int i;
    for (i = 1; i < CHESS_PIECE_COUNT; ++i) {
        if (s_piece_textures[i]) {
            SDL_DestroyTexture(s_piece_textures[i]);
            s_piece_textures[i] = NULL;
        }
    }
    if (s_chess_font) { TTF_CloseFont(s_chess_font); s_chess_font = NULL; TTF_Quit(); }
}

/* ---------------------------------------------------------------------- */

static ChessPlayerColor opposite_color(ChessPlayerColor color)
{
    if (color == CHESS_COLOR_WHITE) {
        return CHESS_COLOR_BLACK;
    }
    if (color == CHESS_COLOR_BLACK) {
        return CHESS_COLOR_WHITE;
    }
    return CHESS_COLOR_UNASSIGNED;
}

static bool use_black_perspective(ChessPlayerColor local_color)
{
    return local_color == CHESS_COLOR_BLACK;
}

static int board_to_screen_index(int idx, bool black_perspective)
{
    return black_perspective ? (CHESS_BOARD_SIZE - 1 - idx) : idx;
}

static int screen_to_board_index(int idx, bool black_perspective)
{
    return black_perspective ? (CHESS_BOARD_SIZE - 1 - idx) : idx;
}

static void render_game_overlay(
    SDL_Renderer *renderer,
    int width,
    int height,
    const ChessGameState *game_state,
    ChessPlayerColor local_color)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(local_color);
    int rank;
    int file;

    if (!renderer || !game_state) {
        return;
    }

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            ChessPiece piece = chess_game_get_piece(game_state, file, rank);
            if (piece == CHESS_PIECE_EMPTY) {
                continue;
            }

            {
                SDL_Texture *tex = ((int)piece > 0 && (int)piece < CHESS_PIECE_COUNT)
                    ? s_piece_textures[(int)piece]
                    : NULL;

                if (tex) {
                    float tex_w = 0.0f;
                    float tex_h = 0.0f;
                    int screen_file = board_to_screen_index(file, black_perspective);
                    int screen_rank = board_to_screen_index(rank, black_perspective);
                    SDL_FRect dst;
                    SDL_GetTextureSize(tex, &tex_w, &tex_h);
                    dst.x = screen_file * cell_w + (cell_w - tex_w) * 0.5f;
                    dst.y = screen_rank * cell_h + (cell_h - tex_h) * 0.5f;
                    dst.w = tex_w;
                    dst.h = tex_h;
                    SDL_RenderTexture(renderer, tex, NULL, &dst);
                } else {
                    /* Fallback: coloured rectangle when font unavailable */
                    int screen_file = board_to_screen_index(file, black_perspective);
                    int screen_rank = board_to_screen_index(rank, black_perspective);
                    SDL_FRect pawn_rect = {
                        screen_file * cell_w + (cell_w * 0.25f),
                        screen_rank * cell_h + (cell_h * 0.25f),
                        cell_w * 0.5f,
                        cell_h * 0.5f
                    };
                    if ((int)piece < (int)CHESS_PIECE_BLACK_PAWN) {
                        SDL_SetRenderDrawColor(renderer, 245, 245, 245, 255);
                    } else {
                        SDL_SetRenderDrawColor(renderer, 25, 25, 25, 255);
                    }
                    SDL_RenderFillRect(renderer, &pawn_rect);
                }
            }
        }
    }

    if (game_state->has_selection) {
        int screen_file = board_to_screen_index(game_state->selected_file, black_perspective);
        int screen_rank = board_to_screen_index(game_state->selected_rank, black_perspective);
        SDL_FRect selected_rect = {
            screen_file * cell_w + 2.0f,
            screen_rank * cell_h + 2.0f,
            cell_w - 4.0f,
            cell_h - 4.0f
        };
        SDL_SetRenderDrawColor(renderer, 255, 204, 0, 255);
        SDL_RenderRect(renderer, &selected_rect);
    }
}

static void process_remote_messages(
    ChessTcpConnection *connection,
    ChessGameState *game_state,
    ChessPlayerColor local_color)
{
    ChessPlayerColor remote_color;

    if (!connection || connection->fd < 0 || !game_state) {
        return;
    }

    remote_color = opposite_color(local_color);
    if (remote_color == CHESS_COLOR_UNASSIGNED) {
        return;
    }

    for (;;) {
        fd_set rfds;
        struct timeval tv = {0, 0};
        int sel;
        ChessPacketHeader header;

        FD_ZERO(&rfds);
        FD_SET(connection->fd, &rfds);
        sel = select(connection->fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            break;
        }

        if (!chess_tcp_recv_packet_header(connection, 1, &header)) {
            SDL_Log("Remote receive failed while reading packet header; closing connection");
            chess_tcp_connection_close(connection);
            break;
        }

        if (header.message_type == CHESS_MSG_MOVE && header.payload_size == sizeof(ChessMovePayload)) {
            ChessMovePayload move;
            if (!chess_tcp_recv_payload(connection, 1, &move, (uint32_t)sizeof(move))) {
                SDL_Log("Remote receive failed while reading MOVE payload; closing connection");
                chess_tcp_connection_close(connection);
                break;
            }

            if (chess_game_apply_remote_move(game_state, remote_color, &move)) {
                SDL_Log(
                    "Applied remote move: (%u,%u) -> (%u,%u)",
                    (unsigned)move.from_file,
                    (unsigned)move.from_rank,
                    (unsigned)move.to_file,
                    (unsigned)move.to_rank
                );
            } else {
                SDL_Log("Ignoring invalid remote MOVE payload");
            }
        } else {
            if (header.payload_size > 4096u) {
                SDL_Log("Remote sent oversized payload (%u); closing connection", (unsigned)header.payload_size);
                chess_tcp_connection_close(connection);
                break;
            }

            if (header.payload_size > 0u) {
                void *discard = malloc((size_t)header.payload_size);
                if (!discard) {
                    SDL_Log("Out of memory while discarding unknown payload; closing connection");
                    chess_tcp_connection_close(connection);
                    break;
                }

                if (!chess_tcp_recv_payload(connection, 1, discard, header.payload_size)) {
                    SDL_Log("Failed to discard unknown payload; closing connection");
                    free(discard);
                    chess_tcp_connection_close(connection);
                    break;
                }

                free(discard);
            }
        }
    }
}

static bool init_local_peer(ChessPeerInfo *local_peer)
{
    if (!local_peer) {
        return false;
    }

    memset(local_peer, 0, sizeof(*local_peer));

    if (!chess_generate_peer_uuid(local_peer->uuid, sizeof(local_peer->uuid))) {
        SDL_Log("Could not generate local peer UUID");
        return false;
    }

    SDL_Log("Local peer initialized (uuid=%s)", local_peer->uuid);
    return true;
}

int app_run(void)
{
    const int window_size = 640;
    const int connect_retry_ms = 1000;
    const int hello_timeout_ms = 1200;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDL3 Chess Board", window_size, window_size, 0);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    init_piece_textures(renderer);

    ChessPeerInfo local_peer;
    ChessNetworkSession network_session;
    ChessDiscoveryContext discovery;
    ChessTcpListener listener;
    ChessTcpConnection connection;
    ChessDiscoveredPeer discovered_peer;
    ChessGameState game_state;
    bool connect_attempted;
    bool hello_completed;
    bool start_completed;
    unsigned int hello_failures;
    unsigned int start_failures;
    uint32_t move_sequence;
    uint64_t next_connect_attempt_at;
    ChessNetworkState last_state;

    if (!init_local_peer(&local_peer)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!chess_tcp_listener_open(&listener, 0)) {
        SDL_Log("Could not create TCP listener on ephemeral port");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Log("TCP listener ready on port %u", (unsigned int)listener.port);

    connection.fd = -1;
    memset(&discovered_peer, 0, sizeof(discovered_peer));
    connect_attempted = false;
    hello_completed = false;
    start_completed = false;
    hello_failures = 0u;
    start_failures = 0u;
    move_sequence = 3u;
    next_connect_attempt_at = 0;
    chess_game_state_init(&game_state);

    if (!chess_discovery_start(&discovery, &local_peer, listener.port)) {
        SDL_Log("Discovery start failed");
        chess_tcp_listener_close(&listener);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    chess_network_session_init(&network_session, &local_peer);

    last_state = network_session.state;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (
                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                network_session.game_started &&
                connection.fd >= 0
            ) {
                int width = 0;
                int height = 0;
                float cell_w;
                float cell_h;
                bool black_perspective;
                int screen_file;
                int screen_rank;
                int file;
                int rank;

                SDL_GetWindowSize(window, &width, &height);
                cell_w = (float)width / (float)CHESS_BOARD_SIZE;
                cell_h = (float)height / (float)CHESS_BOARD_SIZE;
                black_perspective = use_black_perspective(network_session.local_color);
                screen_file = (int)(event.button.x / cell_w);
                screen_rank = (int)(event.button.y / cell_h);
                file = screen_to_board_index(screen_file, black_perspective);
                rank = screen_to_board_index(screen_rank, black_perspective);

                if (file >= 0 && file < CHESS_BOARD_SIZE && rank >= 0 && rank < CHESS_BOARD_SIZE) {
                    if (game_state.has_selection &&
                        game_state.selected_file == file &&
                        game_state.selected_rank == rank) {
                        chess_game_clear_selection(&game_state);
                    } else if (game_state.has_selection) {
                        ChessMovePayload move;
                        if (chess_game_try_local_move(&game_state, network_session.local_color, file, rank, &move)) {
                            if (!chess_tcp_send_packet(
                                &connection,
                                CHESS_MSG_MOVE,
                                move_sequence++,
                                &move,
                                (uint32_t)sizeof(move))) {
                                SDL_Log("Failed to send MOVE packet; closing connection");
                                chess_tcp_connection_close(&connection);
                            } else {
                                SDL_Log(
                                    "Sent local move: (%u,%u) -> (%u,%u)",
                                    (unsigned)move.from_file,
                                    (unsigned)move.from_rank,
                                    (unsigned)move.to_file,
                                    (unsigned)move.to_rank
                                );
                            }
                        }
                    } else {
                        (void)chess_game_select_local_piece(&game_state, network_session.local_color, file, rank);
                    }
                }
            }
        }

        if (!network_session.peer_available) {
            if (chess_discovery_poll(&discovery, &discovered_peer)) {
                chess_network_session_set_remote(&network_session, &discovered_peer.peer);
                SDL_Log(
                    "Peer discovered; starting election (remote port=%u)",
                    (unsigned int)discovered_peer.tcp_port
                );
            }
        }

        if (network_session.state == CHESS_NET_CONNECTING && !hello_completed) {
            const uint64_t now = SDL_GetTicks();
            bool should_attempt = false;

            if (network_session.role == CHESS_ROLE_SERVER) {
                /* Server must accept continuously to drain queued stale connects.
                 * If we only accept once per second while clients timeout at ~500 ms,
                 * every accepted connection is already dead and HELLO always fails. */
                should_attempt = true;
            } else if (network_session.role == CHESS_ROLE_CLIENT) {
                if (!connect_attempted || now >= next_connect_attempt_at) {
                    connect_attempted = true;
                    next_connect_attempt_at = now + (uint64_t)connect_retry_ms;
                    should_attempt = true;
                }
            }

            if (should_attempt) {

                if (network_session.role == CHESS_ROLE_SERVER) {
                    if (connection.fd < 0 && chess_tcp_accept_once(&listener, 10, &connection)) {
                        SDL_Log("Accepted TCP client connection");
                    }
                } else if (network_session.role == CHESS_ROLE_CLIENT) {
                    if (connection.fd < 0 &&
                        chess_tcp_connect_once(
                            network_session.remote_peer.ipv4_host_order,
                            discovered_peer.tcp_port,
                            200,
                            &connection
                        )) {
                        SDL_Log("Connected to remote TCP host");
                    }
                }

                if (connection.fd >= 0) {
                    ChessHelloPayload local_hello;
                    ChessHelloPayload remote_hello;
                    ChessAckPayload handshake_ack;
                    bool handshake_ok = false;

                    memset(&local_hello, 0, sizeof(local_hello));
                    memset(&remote_hello, 0, sizeof(remote_hello));
                    memset(&handshake_ack, 0, sizeof(handshake_ack));
                    SDL_strlcpy(local_hello.uuid, network_session.local_peer.uuid, sizeof(local_hello.uuid));
                    local_hello.role = (uint32_t)network_session.role;

                    if (network_session.role == CHESS_ROLE_CLIENT) {
                        /* CLIENT: send -> recv -> send_ack
                         * Ordering ensures SERVER's recv_ack catches stale connections:
                         * if CLIENT already closed, SERVER's recv_ack gets EOF -> fail. */
                        handshake_ok =
                            chess_tcp_send_hello(&connection, &local_hello) &&
                            chess_tcp_recv_hello(&connection, hello_timeout_ms, &remote_hello) &&
                            chess_tcp_send_ack(&connection, CHESS_MSG_HELLO, 1u, 0u);
                    } else {
                        /* SERVER: recv -> send -> recv_ack */
                        handshake_ok =
                            chess_tcp_recv_hello(&connection, hello_timeout_ms, &remote_hello) &&
                            chess_tcp_send_hello(&connection, &local_hello) &&
                            chess_tcp_recv_ack(&connection, hello_timeout_ms, &handshake_ack) &&
                            handshake_ack.acked_message_type == CHESS_MSG_HELLO &&
                            handshake_ack.acked_sequence == 1u &&
                            handshake_ack.status_code == 0u;
                    }

                    if (handshake_ok) {
                        hello_completed = true;
                        chess_network_session_set_transport_ready(&network_session, true);
                        SDL_Log("HELLO handshake completed with peer uuid=%s", remote_hello.uuid);
                    } else {
                        hello_failures += 1u;
                        if (hello_failures == 1u || (hello_failures % 5u) == 0u) {
                            SDL_Log(
                                "HELLO handshake failed (%u failures), will retry connection",
                                hello_failures
                            );
                        }
                        chess_tcp_connection_close(&connection);
                    }
                }
            }
        }

        if (network_session.state == CHESS_NET_IN_GAME && hello_completed && !start_completed) {
            ChessStartPayload start_payload;
            ChessAckPayload start_ack;
            bool start_ok = false;

            memset(&start_payload, 0, sizeof(start_payload));
            memset(&start_ack, 0, sizeof(start_ack));

            if (network_session.role == CHESS_ROLE_SERVER) {
                start_payload.game_id = make_game_id(&network_session.local_peer, &network_session.remote_peer);
                start_payload.assigned_color = CHESS_COLOR_BLACK;
                start_payload.initial_turn = CHESS_COLOR_WHITE;
                SDL_strlcpy(start_payload.white_uuid, network_session.local_peer.uuid, sizeof(start_payload.white_uuid));
                SDL_strlcpy(start_payload.black_uuid, network_session.remote_peer.uuid, sizeof(start_payload.black_uuid));

                start_ok =
                    chess_tcp_send_start(&connection, &start_payload) &&
                    chess_tcp_recv_ack(&connection, 500, &start_ack) &&
                    start_ack.acked_message_type == CHESS_MSG_START &&
                    start_ack.acked_sequence == 2u &&
                    start_ack.status_code == 0u;

                if (start_ok) {
                    chess_network_session_start_game(&network_session, start_payload.game_id, CHESS_COLOR_WHITE);
                }
            } else if (network_session.role == CHESS_ROLE_CLIENT) {
                start_ok =
                    chess_tcp_recv_start(&connection, 500, &start_payload) &&
                    chess_tcp_send_ack(&connection, CHESS_MSG_START, 2u, 0u);

                if (start_ok) {
                    chess_network_session_start_game(
                        &network_session,
                        start_payload.game_id,
                        (ChessPlayerColor)start_payload.assigned_color
                    );
                }
            }

            if (start_ok) {
                start_completed = true;
                chess_game_state_init(&game_state);
                SDL_Log(
                    "Game started (game_id=%u, local_color=%s, first_turn=%s)",
                    network_session.game_id,
                    network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
                    start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
                );
            } else {
                start_failures += 1u;
                if (start_failures == 1u || (start_failures % 5u) == 0u) {
                    SDL_Log("START exchange failed (%u failures), will retry", start_failures);
                }
            }
        }

        if (network_session.game_started && connection.fd >= 0) {
            process_remote_messages(&connection, &game_state, network_session.local_color);
        }

        chess_network_session_step(&network_session);

        if (network_session.state != last_state) {
            SDL_Log(
                "Network state changed: %s -> %s",
                network_state_to_string(last_state),
                network_state_to_string(network_session.state)
            );

            if (network_session.state == CHESS_NET_CONNECTING) {
                if (network_session.role == CHESS_ROLE_SERVER) {
                    SDL_Log("Local role: SERVER (smaller IP)");
                } else if (network_session.role == CHESS_ROLE_CLIENT) {
                    SDL_Log("Local role: CLIENT");
                }
            }

            last_state = network_session.state;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_board(renderer, width, height);
        render_game_overlay(renderer, width, height, &game_state, network_session.local_color);

        SDL_RenderPresent(renderer);
    }

    chess_discovery_stop(&discovery);
    chess_tcp_connection_close(&connection);
    chess_tcp_listener_close(&listener);

    destroy_piece_textures();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
