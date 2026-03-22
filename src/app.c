#include "chess_app/app.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"

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
static TTF_Font    *s_coord_font                     = NULL;
static SDL_Texture *s_piece_textures[CHESS_PIECE_COUNT];
static SDL_Texture *s_file_label_textures[CHESS_BOARD_SIZE][2];
static SDL_Texture *s_rank_label_textures[CHESS_BOARD_SIZE][2];
static bool         s_ttf_initialized                = false;

static TTF_Font *open_font_from_candidates(const char * const *font_paths, float font_size)
{
    int i;

    for (i = 0; font_paths[i] != NULL; ++i) {
        TTF_Font *font = TTF_OpenFont(font_paths[i], font_size);
        if (font) {
            SDL_Log("Loaded font: %s (size=%.1f)", font_paths[i], (double)font_size);
            return font;
        }
    }
    return NULL;
}

static SDL_Texture *make_text_texture(SDL_Renderer *renderer, TTF_Font *font, const char *text, SDL_Color color)
{
    SDL_Surface *surface;
    SDL_Texture *texture;

    if (!renderer || !font || !text) {
        return NULL;
    }

    surface = TTF_RenderText_Blended(font, text, SDL_strlen(text), color);
    if (!surface) {
        return NULL;
    }

    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

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
    const SDL_Color coord_on_light = {60, 70, 45, 255};
    const SDL_Color coord_on_dark  = {238, 238, 210, 255};
    int i;
    float font_size = 52.0f;

    if (!TTF_Init()) {
        SDL_Log("TTF_Init failed: %s", SDL_GetError());
        return;
    }
    s_ttf_initialized = true;

    s_chess_font = open_font_from_candidates(font_paths, font_size);
    if (!s_chess_font) {
        SDL_Log("No chess font found, piece rendering will use fallback rectangles");
    }

    if (s_chess_font) {
        for (i = 1; i < CHESS_PIECE_COUNT; ++i) {
            bool is_white = (i < (int)CHESS_PIECE_BLACK_PAWN);
            SDL_Color fg      = is_white ? white_fg      : black_fg;
            SDL_Color outline = is_white ? white_outline : black_outline;
            s_piece_textures[i] = make_outlined_glyph_texture(
                renderer, s_chess_font, piece_codepoints[i], fg, outline, 2);
            if (!s_piece_textures[i]) {
                SDL_Log("Failed to create texture for piece %d: %s", i, SDL_GetError());
            }
        }
    }

    s_coord_font = open_font_from_candidates(font_paths, 16.0f);
    if (!s_coord_font) {
        SDL_Log("No coordinate font found, board coordinates disabled");
    } else {
        for (i = 0; i < CHESS_BOARD_SIZE; ++i) {
            char file_label[2] = { (char)('a' + i), '\0' };
            char rank_label[2] = { (char)('8' - i), '\0' };

            s_file_label_textures[i][0] = make_text_texture(renderer, s_coord_font, file_label, coord_on_light);
            s_file_label_textures[i][1] = make_text_texture(renderer, s_coord_font, file_label, coord_on_dark);
            s_rank_label_textures[i][0] = make_text_texture(renderer, s_coord_font, rank_label, coord_on_light);
            s_rank_label_textures[i][1] = make_text_texture(renderer, s_coord_font, rank_label, coord_on_dark);
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

    for (i = 0; i < CHESS_BOARD_SIZE; ++i) {
        if (s_file_label_textures[i][0]) {
            SDL_DestroyTexture(s_file_label_textures[i][0]);
            s_file_label_textures[i][0] = NULL;
        }
        if (s_file_label_textures[i][1]) {
            SDL_DestroyTexture(s_file_label_textures[i][1]);
            s_file_label_textures[i][1] = NULL;
        }
        if (s_rank_label_textures[i][0]) {
            SDL_DestroyTexture(s_rank_label_textures[i][0]);
            s_rank_label_textures[i][0] = NULL;
        }
        if (s_rank_label_textures[i][1]) {
            SDL_DestroyTexture(s_rank_label_textures[i][1]);
            s_rank_label_textures[i][1] = NULL;
        }
    }

    if (s_chess_font) {
        TTF_CloseFont(s_chess_font);
        s_chess_font = NULL;
    }
    if (s_coord_font) {
        TTF_CloseFont(s_coord_font);
        s_coord_font = NULL;
    }
    if (s_ttf_initialized) {
        TTF_Quit();
        s_ttf_initialized = false;
    }
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

static bool board_square_is_light(int file, int rank)
{
    return ((file + rank) % 2) == 0;
}

static void render_board_coordinates(
    SDL_Renderer *renderer,
    int width,
    int height,
    ChessPlayerColor local_color)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(local_color);
    int screen_file;
    int screen_rank;

    if (!renderer || !s_coord_font) {
        return;
    }

    for (screen_file = 0; screen_file < CHESS_BOARD_SIZE; ++screen_file) {
        int board_file = screen_to_board_index(screen_file, black_perspective);
        int board_rank = screen_to_board_index(CHESS_BOARD_SIZE - 1, black_perspective);
        int color_idx = board_square_is_light(board_file, board_rank) ? 0 : 1;
        SDL_Texture *label_tex = s_file_label_textures[board_file][color_idx];
        if (label_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(label_tex, &tex_w, &tex_h);
            dst.x = screen_file * cell_w + cell_w - tex_w - 4.0f;
            dst.y = (CHESS_BOARD_SIZE - 1) * cell_h + cell_h - tex_h - 2.0f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, label_tex, NULL, &dst);
        }
    }

    for (screen_rank = 0; screen_rank < CHESS_BOARD_SIZE; ++screen_rank) {
        int board_rank = screen_to_board_index(screen_rank, black_perspective);
        int board_file = screen_to_board_index(0, black_perspective);
        int color_idx = board_square_is_light(board_file, board_rank) ? 0 : 1;
        SDL_Texture *label_tex = s_rank_label_textures[board_rank][color_idx];
        if (label_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(label_tex, &tex_w, &tex_h);
            dst.x = 4.0f;
            dst.y = screen_rank * cell_h + 2.0f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, label_tex, NULL, &dst);
        }
    }
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

typedef struct ChessSocketEvents {
    bool listener_readable;
    bool connection_readable;
    bool connection_writable;
} ChessSocketEvents;

static void poll_socket_events(
    const ChessTcpListener *listener,
    const ChessTcpConnection *connection,
    ChessSocketEvents *events)
{
    fd_set rfds;
    fd_set wfds;
    struct timeval tv = {0, 0};
    int maxfd = -1;
    int sel;

    if (!events) {
        return;
    }

    memset(events, 0, sizeof(*events));
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    if (listener && listener->fd >= 0) {
        FD_SET(listener->fd, &rfds);
        if (listener->fd > maxfd) {
            maxfd = listener->fd;
        }
    }

    if (connection && connection->fd >= 0) {
        FD_SET(connection->fd, &rfds);
        FD_SET(connection->fd, &wfds);
        if (connection->fd > maxfd) {
            maxfd = connection->fd;
        }
    }

    if (maxfd < 0) {
        return;
    }

    sel = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
    if (sel <= 0) {
        return;
    }

    if (listener && listener->fd >= 0 && FD_ISSET(listener->fd, &rfds)) {
        events->listener_readable = true;
    }
    if (connection && connection->fd >= 0) {
        events->connection_readable = FD_ISSET(connection->fd, &rfds);
        events->connection_writable = FD_ISSET(connection->fd, &wfds);
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

static void render_lobby(
    SDL_Renderer *renderer,
    int width,
    int height,
    const ChessLobbyState *lobby,
    TTF_Font *font)
{
    const int peer_row_height = 60;
    const int margin = 10;
    const int peer_item_width = 400;
    const int peer_item_x = (width - peer_item_width) / 2;
    int i;
    int y = margin + 20;

    if (!renderer || !lobby || !font) {
        return;
    }

    /* Title */
    {
        SDL_Texture *title_tex = make_text_texture(
            renderer, font, "Discover players - Click to challenge", (SDL_Color){238, 238, 210, 255});
        if (title_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(title_tex, &tex_w, &tex_h);
            dst.x = (float)(width - (int)tex_w) / 2.0f;
            dst.y = (float)margin;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, title_tex, NULL, &dst);
            SDL_DestroyTexture(title_tex);
        }
    }

    y = margin + 50;

    /* Render peer list */
    for (i = 0; i < lobby->discovered_peer_count; ++i) {
        const ChessDiscoveredPeerState *peer_state = &lobby->discovered_peers[i];
        const SDL_Color bg_color = (i == lobby->selected_peer_idx)
            ? (SDL_Color){100, 150, 200, 255}
            : (SDL_Color){60, 60, 60, 255};
        const SDL_Color text_color = (SDL_Color){238, 238, 210, 255};
        SDL_FRect peer_rect = {
            (float)peer_item_x,
            (float)y,
            (float)peer_item_width,
            (float)peer_row_height
        };

        /* Draw background rectangle */
        SDL_SetRenderDrawColor(
            renderer,
            bg_color.r,
            bg_color.g,
            bg_color.b,
            bg_color.a
        );
        SDL_RenderFillRect(renderer, &peer_rect);

        /* Draw border */
        SDL_SetRenderDrawColor(renderer, 180, 180, 180, 255);
        SDL_RenderRect(renderer, &peer_rect);

        /* Draw peer UUID and challenge state */
        {
            char peer_label[128];
            const char *challenge_icon = "";

            switch (peer_state->challenge_state) {
            case CHESS_CHALLENGE_NONE:
                challenge_icon = "";
                break;
            case CHESS_CHALLENGE_OUTGOING_PENDING:
                challenge_icon = " [⏳]";
                break;
            case CHESS_CHALLENGE_INCOMING_PENDING:
                challenge_icon = " [⚔]";
                break;
            case CHESS_CHALLENGE_MATCHED:
                challenge_icon = " [✓]";
                break;
            }

            SDL_snprintf(
                peer_label,
                sizeof(peer_label),
                "%.8s...%s",
                peer_state->peer.uuid,
                challenge_icon
            );

            SDL_Texture *label_tex = make_text_texture(renderer, font, peer_label, text_color);
            if (label_tex) {
                float tex_w = 0.0f;
                float tex_h = 0.0f;
                SDL_FRect dst;
                SDL_GetTextureSize(label_tex, &tex_w, &tex_h);
                dst.x = peer_rect.x + 15.0f;
                dst.y = peer_rect.y + (peer_rect.h - tex_h) / 2.0f;
                dst.w = tex_w;
                dst.h = tex_h;
                SDL_RenderTexture(renderer, label_tex, NULL, &dst);
                SDL_DestroyTexture(label_tex);
            }
        }

        y += peer_row_height;
    }

    /* If no peers, show waiting message */
    if (lobby->discovered_peer_count == 0) {
        SDL_Texture *waiting_tex = make_text_texture(
            renderer, font, "Scanning for opponents...", (SDL_Color){180, 180, 180, 255});
        if (waiting_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(waiting_tex, &tex_w, &tex_h);
            dst.x = (float)(width - (int)tex_w) / 2.0f;
            dst.y = (float)(height - (int)tex_h) / 2.0f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, waiting_tex, NULL, &dst);
            SDL_DestroyTexture(waiting_tex);
        }
    }
}

typedef struct AppLoopContext {
    int window_size;
    int connect_retry_ms;
    SDL_Window *window;
    SDL_Renderer *renderer;
    ChessPeerInfo local_peer;
    ChessNetworkSession network_session;
    ChessDiscoveryContext discovery;
    ChessTcpListener listener;
    ChessTcpConnection connection;
    ChessDiscoveredPeer discovered_peer;
    ChessGameState game_state;
    ChessLobbyState lobby;
    bool connect_attempted;
    bool hello_sent;
    bool hello_received;
    bool hello_ack_sent;
    bool hello_ack_received;
    bool hello_completed;
    bool challenge_exchange_completed;
    bool start_sent;
    bool start_completed;
    ChessStartPayload pending_start_payload;
    unsigned int start_failures;
    uint32_t move_sequence;
    uint64_t next_connect_attempt_at;
    ChessNetworkState last_state;
    bool running;
} AppLoopContext;

static void reset_transport_progress(AppLoopContext *ctx);

static void app_loop_context_init_defaults(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->window_size = 640;
    ctx->connect_retry_ms = 1000;
    ctx->connection.fd = -1;
    ctx->running = true;
}

static void app_loop_context_shutdown(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    chess_discovery_stop(&ctx->discovery);
    chess_tcp_connection_close(&ctx->connection);
    chess_tcp_listener_close(&ctx->listener);
    destroy_piece_textures();
    if (ctx->renderer) {
        SDL_DestroyRenderer(ctx->renderer);
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
    }
    SDL_Quit();
}

static bool init_app_window_and_renderer(AppLoopContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    ctx->window = SDL_CreateWindow("SDL3 Chess Board", ctx->window_size, ctx->window_size, 0);
    if (!ctx->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
        SDL_Quit();
        return false;
    }

    init_piece_textures(ctx->renderer);
    return true;
}

static void init_app_runtime_state(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->connection.fd = -1;
    memset(&ctx->discovered_peer, 0, sizeof(ctx->discovered_peer));
    ctx->start_completed = false;
    memset(&ctx->pending_start_payload, 0, sizeof(ctx->pending_start_payload));
    ctx->start_failures = 0u;
    ctx->move_sequence = 3u;
    ctx->next_connect_attempt_at = 0;

    reset_transport_progress(ctx);
    chess_game_state_init(&ctx->game_state);
    chess_lobby_init(&ctx->lobby);
}

static bool init_app_networking(AppLoopContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!init_local_peer(&ctx->local_peer)) {
        return false;
    }

    if (!chess_tcp_listener_open(&ctx->listener, 0)) {
        SDL_Log("Could not create TCP listener on ephemeral port");
        return false;
    }

    SDL_Log("TCP listener ready on port %u", (unsigned int)ctx->listener.port);

    init_app_runtime_state(ctx);

    if (!chess_discovery_start(&ctx->discovery, &ctx->local_peer, ctx->listener.port)) {
        SDL_Log("Discovery start failed");
        return false;
    }

    chess_network_session_init(&ctx->network_session, &ctx->local_peer);
    ctx->last_state = ctx->network_session.state;
    return true;
}

static int find_clicked_lobby_peer(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    const int peer_row_height = 60;
    const int margin = 10;
    const int peer_item_width = 400;
    int width = 0;
    int height = 0;
    int peer_item_x;
    int lobby_start_y;
    int peer_idx;

    if (!ctx || !ctx->window) {
        return -1;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    peer_item_x = (width - peer_item_width) / 2;
    lobby_start_y = margin + 50;

    for (peer_idx = 0; peer_idx < ctx->lobby.discovered_peer_count; ++peer_idx) {
        const int peer_y = lobby_start_y + peer_idx * peer_row_height;
        if (mouse_x >= peer_item_x &&
            mouse_x < peer_item_x + peer_item_width &&
            mouse_y >= peer_y &&
            mouse_y < peer_y + peer_row_height) {
            return peer_idx;
        }
    }

    return -1;
}

static void handle_lobby_click(AppLoopContext *ctx, int clicked_peer)
{
    if (!ctx || clicked_peer < 0 || clicked_peer >= ctx->lobby.discovered_peer_count) {
        return;
    }

    if (ctx->lobby.selected_peer_idx != clicked_peer) {
        ctx->lobby.selected_peer_idx = clicked_peer;
        SDL_Log("Selected peer %d", clicked_peer);
        return;
    }

    {
        ChessChallengeState current_state = chess_lobby_get_challenge_state(&ctx->lobby, clicked_peer);

        if (current_state == CHESS_CHALLENGE_NONE) {
            chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_OUTGOING_PENDING);
            chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[clicked_peer].peer);
            SDL_Log("Challenge sent to peer %d (%.8s...)", clicked_peer, ctx->lobby.discovered_peers[clicked_peer].peer.uuid);
        } else if (current_state == CHESS_CHALLENGE_OUTGOING_PENDING) {
            chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_NONE);
            SDL_Log("Challenge cancelled for peer %d", clicked_peer);
        } else if (current_state == CHESS_CHALLENGE_INCOMING_PENDING) {
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_uuid, ctx->network_session.local_peer.uuid, sizeof(accept.acceptor_uuid));

            if (ctx->connection.fd >= 0 && chess_tcp_send_accept(&ctx->connection, &accept)) {
                ctx->challenge_exchange_completed = true;
                chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_MATCHED);
                chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[clicked_peer].peer);
                SDL_Log("Accepted challenge from peer %d (%.8s...)", clicked_peer, ctx->lobby.discovered_peers[clicked_peer].peer.uuid);
                SDL_Log("Challenge exchange completed (local accept), waiting START/ACK");
            } else {
                SDL_Log("Cannot accept challenge yet: transport not ready");
            }
        }
    }
}

static void handle_board_click(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    int width = 0;
    int height = 0;
    float cell_w;
    float cell_h;
    bool black_perspective;
    int screen_file;
    int screen_rank;
    int file;
    int rank;

    if (!ctx || !ctx->window || ctx->connection.fd < 0) {
        return;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    cell_h = (float)height / (float)CHESS_BOARD_SIZE;
    black_perspective = use_black_perspective(ctx->network_session.local_color);
    screen_file = (int)(mouse_x / cell_w);
    screen_rank = (int)(mouse_y / cell_h);
    file = screen_to_board_index(screen_file, black_perspective);
    rank = screen_to_board_index(screen_rank, black_perspective);

    if (file < 0 || file >= CHESS_BOARD_SIZE || rank < 0 || rank >= CHESS_BOARD_SIZE) {
        return;
    }

    if (ctx->game_state.has_selection &&
        ctx->game_state.selected_file == file &&
        ctx->game_state.selected_rank == rank) {
        chess_game_clear_selection(&ctx->game_state);
        return;
    }

    if (ctx->game_state.has_selection) {
        ChessMovePayload move;
        if (chess_game_try_local_move(&ctx->game_state, ctx->network_session.local_color, file, rank, &move)) {
            if (!chess_tcp_send_packet(
                    &ctx->connection,
                    CHESS_MSG_MOVE,
                    ctx->move_sequence++,
                    &move,
                    (uint32_t)sizeof(move))) {
                SDL_Log("Failed to send MOVE packet; closing connection");
                chess_tcp_connection_close(&ctx->connection);
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
        return;
    }

    (void)chess_game_select_local_piece(&ctx->game_state, ctx->network_session.local_color, file, rank);
}

static void handle_sdl_events(AppLoopContext *ctx)
{
    SDL_Event event;

    if (!ctx) {
        return;
    }

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            ctx->running = false;
            continue;
        }

        if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT) {
            continue;
        }

        if (!ctx->network_session.game_started && ctx->lobby.discovered_peer_count > 0) {
            const int clicked_peer = find_clicked_lobby_peer(ctx, event.button.x, event.button.y);
            if (clicked_peer >= 0) {
                handle_lobby_click(ctx, clicked_peer);
            }
            continue;
        }

        if (ctx->network_session.game_started && ctx->connection.fd >= 0) {
            handle_board_click(ctx, event.button.x, event.button.y);
        }
    }
}

static void poll_discovery_and_update_lobby(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (!ctx->network_session.peer_available) {
        if (chess_discovery_poll(&ctx->discovery, &ctx->discovered_peer)) {
            chess_lobby_add_or_update_peer(&ctx->lobby, &ctx->discovered_peer.peer, ctx->discovered_peer.tcp_port);
            chess_network_session_set_remote(&ctx->network_session, &ctx->discovered_peer.peer);
            SDL_Log(
                "New peer discovered: %.8s... (port=%u)",
                ctx->discovered_peer.peer.uuid,
                (unsigned int)ctx->discovered_peer.tcp_port
            );
        }
    }
}

static void render_frame(AppLoopContext *ctx)
{
    int width = 0;
    int height = 0;

    if (!ctx || !ctx->renderer || !ctx->window) {
        return;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->renderer);

    if (!ctx->network_session.game_started) {
        render_lobby(ctx->renderer, width, height, &ctx->lobby, s_coord_font);
    } else {
        render_board(ctx->renderer, width, height);
        render_game_overlay(ctx->renderer, width, height, &ctx->game_state, ctx->network_session.local_color);
        render_board_coordinates(ctx->renderer, width, height, ctx->network_session.local_color);
    }

    SDL_RenderPresent(ctx->renderer);
}

static void log_network_state_transition(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->network_session.state == ctx->last_state) {
        return;
    }

    SDL_Log(
        "Network state changed: %s -> %s",
        network_state_to_string(ctx->last_state),
        network_state_to_string(ctx->network_session.state)
    );

    if (ctx->network_session.state == CHESS_NET_CONNECTING) {
        if (ctx->network_session.role == CHESS_ROLE_SERVER) {
            SDL_Log("Local role: SERVER (smaller IP)");
        } else if (ctx->network_session.role == CHESS_ROLE_CLIENT) {
            SDL_Log("Local role: CLIENT");
        }
    }

    ctx->last_state = ctx->network_session.state;
}

static void reset_transport_progress(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->connect_attempted = false;
    ctx->hello_sent = false;
    ctx->hello_received = false;
    ctx->hello_ack_sent = false;
    ctx->hello_ack_received = false;
    ctx->hello_completed = false;
    ctx->challenge_exchange_completed = false;
    ctx->start_sent = false;
}

static bool receive_next_packet(AppLoopContext *ctx, ChessPacketHeader *header, uint8_t *payload, size_t payload_capacity)
{
    if (!ctx || !header || !payload) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(&ctx->connection, 1, header)) {
        SDL_Log("Failed to read packet header; closing connection");
        chess_tcp_connection_close(&ctx->connection);
        reset_transport_progress(ctx);
        return false;
    }

    if (header->payload_size > payload_capacity) {
        SDL_Log("Received oversized payload (%u); closing connection", (unsigned)header->payload_size);
        chess_tcp_connection_close(&ctx->connection);
        reset_transport_progress(ctx);
        return false;
    }

    if (header->payload_size > 0u && !chess_tcp_recv_payload(&ctx->connection, 1, payload, header->payload_size)) {
        SDL_Log("Failed to read packet payload; closing connection");
        chess_tcp_connection_close(&ctx->connection);
        reset_transport_progress(ctx);
        return false;
    }

    return true;
}

static void handle_incoming_packet(AppLoopContext *ctx, const ChessPacketHeader *header, const uint8_t *payload)
{
    if (!ctx || !header || !payload) {
        return;
    }

    if (header->message_type == CHESS_MSG_HELLO && header->payload_size == sizeof(ChessHelloPayload)) {
        const ChessHelloPayload *hello = (const ChessHelloPayload *)payload;
        ctx->hello_received = true;
        SDL_Log("Received HELLO from remote peer (%.8s...)", hello->uuid);
    } else if (header->message_type == CHESS_MSG_OFFER && header->payload_size == sizeof(ChessOfferPayload)) {
        if (!ctx->challenge_exchange_completed) {
            const ChessOfferPayload *offer = (const ChessOfferPayload *)payload;
            int peer_idx = -1;
            int i;

            SDL_Log("Received OFFER from remote peer (%.8s...)", offer->challenger_uuid);

            for (i = 0; i < ctx->lobby.discovered_peer_count; ++i) {
                if (SDL_strncmp(ctx->lobby.discovered_peers[i].peer.uuid, offer->challenger_uuid, CHESS_UUID_STRING_LEN) == 0) {
                    peer_idx = i;
                    break;
                }
            }

            if (peer_idx >= 0) {
                chess_lobby_set_challenge_state(&ctx->lobby, peer_idx, CHESS_CHALLENGE_INCOMING_PENDING);
                chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[peer_idx].peer);
            }
        }
    } else if (header->message_type == CHESS_MSG_ACCEPT && header->payload_size == sizeof(ChessAcceptPayload)) {
        if (!ctx->challenge_exchange_completed) {
            const ChessAcceptPayload *accept = (const ChessAcceptPayload *)payload;
            int peer_idx = ctx->lobby.selected_peer_idx;
            int i;

            if (peer_idx < 0) {
                for (i = 0; i < ctx->lobby.discovered_peer_count; ++i) {
                    if (SDL_strncmp(
                            ctx->lobby.discovered_peers[i].peer.uuid,
                            ctx->network_session.remote_peer.uuid,
                            CHESS_UUID_STRING_LEN) == 0) {
                        peer_idx = i;
                        break;
                    }
                }
            }

            ctx->challenge_exchange_completed = true;
            if (peer_idx >= 0) {
                chess_lobby_set_challenge_state(&ctx->lobby, peer_idx, CHESS_CHALLENGE_MATCHED);
            }
            SDL_Log("Received ACCEPT from remote peer (%.8s...)", accept->acceptor_uuid);
            SDL_Log("Challenge exchange completed (remote accept), waiting START/ACK");
        }
    } else if (header->message_type == CHESS_MSG_START && header->payload_size == sizeof(ChessStartPayload)) {
        if (ctx->network_session.role == CHESS_ROLE_CLIENT && !ctx->start_completed) {
            const ChessStartPayload *start_payload = (const ChessStartPayload *)payload;
            if (chess_tcp_send_ack(&ctx->connection, CHESS_MSG_START, 2u, 0u)) {
                chess_network_session_start_game(
                    &ctx->network_session,
                    start_payload->game_id,
                    (ChessPlayerColor)start_payload->assigned_color
                );
                ctx->start_completed = true;
                chess_game_state_init(&ctx->game_state);
                SDL_Log(
                    "Game started (game_id=%u, local_color=%s, first_turn=%s)",
                    ctx->network_session.game_id,
                    ctx->network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
                    start_payload->initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
                );
            }
        }
    } else if (header->message_type == CHESS_MSG_ACK && header->payload_size == sizeof(ChessAckPayload)) {
        const ChessAckPayload *ack = (const ChessAckPayload *)payload;
        if (ctx->network_session.role == CHESS_ROLE_SERVER &&
            ack->acked_message_type == CHESS_MSG_HELLO &&
            ack->acked_sequence == 1u &&
            ack->status_code == 0u) {
            ctx->hello_ack_received = true;
        } else if (ctx->network_session.role == CHESS_ROLE_SERVER && ctx->start_sent && !ctx->start_completed) {
            if (ack->acked_message_type == CHESS_MSG_START &&
                ack->acked_sequence == 2u &&
                ack->status_code == 0u) {
                SDL_Log("START ACK received, switching to game view");
                chess_network_session_start_game(&ctx->network_session, ctx->pending_start_payload.game_id, CHESS_COLOR_WHITE);
                ctx->start_completed = true;
                chess_game_state_init(&ctx->game_state);
                SDL_Log(
                    "Game started (game_id=%u, local_color=%s, first_turn=%s)",
                    ctx->network_session.game_id,
                    ctx->network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
                    ctx->pending_start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
                );
            }
        }
    } else if (header->message_type == CHESS_MSG_MOVE && header->payload_size == sizeof(ChessMovePayload)) {
        if (ctx->network_session.game_started) {
            const ChessMovePayload *move = (const ChessMovePayload *)payload;
            ChessPlayerColor remote_color = opposite_color(ctx->network_session.local_color);
            if (remote_color != CHESS_COLOR_UNASSIGNED) {
                if (chess_game_apply_remote_move(&ctx->game_state, remote_color, move)) {
                    SDL_Log(
                        "Applied remote move: (%u,%u) -> (%u,%u)",
                        (unsigned)move->from_file,
                        (unsigned)move->from_rank,
                        (unsigned)move->to_file,
                        (unsigned)move->to_rank
                    );
                } else {
                    SDL_Log("Ignoring invalid remote MOVE payload");
                }
            }
        }
    }
}

static void drain_incoming_packets(AppLoopContext *ctx)
{
    const int max_packets_per_frame = 8;
    int packet_idx;

    if (!ctx || ctx->connection.fd < 0) {
        return;
    }

    for (packet_idx = 0; packet_idx < max_packets_per_frame; ++packet_idx) {
        ChessSocketEvents drain_events;
        ChessPacketHeader header;
        uint8_t payload[4096];

        poll_socket_events(&ctx->listener, &ctx->connection, &drain_events);
        if (!drain_events.connection_readable) {
            break;
        }

        if (!receive_next_packet(ctx, &header, payload, sizeof(payload))) {
            break;
        }

        handle_incoming_packet(ctx, &header, payload);
    }
}

static void advance_transport_connection(AppLoopContext *ctx, const ChessSocketEvents *socket_events)
{
    if (!ctx || !socket_events) {
        return;
    }

    if (ctx->network_session.state != CHESS_NET_CONNECTING || ctx->hello_completed) {
        return;
    }

    {
        const uint64_t now = SDL_GetTicks();
        bool should_attempt_client_connect = false;

        if (ctx->network_session.role == CHESS_ROLE_CLIENT) {
            if (!ctx->connect_attempted || now >= ctx->next_connect_attempt_at) {
                ctx->connect_attempted = true;
                ctx->next_connect_attempt_at = now + (uint64_t)ctx->connect_retry_ms;
                should_attempt_client_connect = true;
            }
        }

        if (ctx->network_session.role == CHESS_ROLE_SERVER &&
            ctx->connection.fd < 0 &&
            socket_events->listener_readable) {
            if (chess_tcp_accept_once(&ctx->listener, 0, &ctx->connection)) {
                SDL_Log("Accepted TCP client connection");
            }
        } else if (ctx->network_session.role == CHESS_ROLE_CLIENT && should_attempt_client_connect) {
            uint16_t remote_port = ctx->discovered_peer.tcp_port;
            if (ctx->lobby.selected_peer_idx >= 0 &&
                ctx->lobby.selected_peer_idx < ctx->lobby.discovered_peer_count) {
                remote_port = ctx->lobby.discovered_peers[ctx->lobby.selected_peer_idx].tcp_port;
            }

            if (ctx->connection.fd < 0 &&
                chess_tcp_connect_once(
                    ctx->network_session.remote_peer.ipv4_host_order,
                    remote_port,
                    200,
                    &ctx->connection
                )) {
                SDL_Log("Connected to remote TCP host");
            }
        }
    }
}

static void advance_hello_handshake(AppLoopContext *ctx)
{
    if (!ctx || ctx->connection.fd < 0) {
        return;
    }

    if (ctx->network_session.state != CHESS_NET_CONNECTING || ctx->hello_completed) {
        return;
    }

    {
        ChessHelloPayload local_hello;
        memset(&local_hello, 0, sizeof(local_hello));
        SDL_strlcpy(local_hello.uuid, ctx->network_session.local_peer.uuid, sizeof(local_hello.uuid));
        local_hello.role = (uint32_t)ctx->network_session.role;

        if (ctx->network_session.role == CHESS_ROLE_CLIENT && !ctx->hello_sent) {
            if (chess_tcp_send_hello(&ctx->connection, &local_hello)) {
                ctx->hello_sent = true;
            }
        }

        if (ctx->network_session.role == CHESS_ROLE_CLIENT && ctx->hello_received && !ctx->hello_ack_sent) {
            if (chess_tcp_send_ack(&ctx->connection, CHESS_MSG_HELLO, 1u, 0u)) {
                ctx->hello_ack_sent = true;
            }
        }

        if (ctx->network_session.role == CHESS_ROLE_SERVER && ctx->hello_received && !ctx->hello_sent) {
            if (chess_tcp_send_hello(&ctx->connection, &local_hello)) {
                ctx->hello_sent = true;
            }
        }

        if (ctx->network_session.role == CHESS_ROLE_CLIENT &&
            ctx->hello_sent &&
            ctx->hello_received &&
            ctx->hello_ack_sent) {
            ctx->hello_completed = true;
            chess_network_session_set_transport_ready(&ctx->network_session, true);
            SDL_Log("HELLO handshake completed (client)");
        } else if (ctx->network_session.role == CHESS_ROLE_SERVER &&
                   ctx->hello_received &&
                   ctx->hello_sent &&
                   ctx->hello_ack_received) {
            ctx->hello_completed = true;
            chess_network_session_set_transport_ready(&ctx->network_session, true);
            SDL_Log("HELLO handshake completed (server)");
        }
    }
}

static void send_pending_offer_if_needed(AppLoopContext *ctx)
{
    if (!ctx || !ctx->hello_completed || ctx->challenge_exchange_completed || ctx->connection.fd < 0) {
        return;
    }

    {
        const int peer_idx = ctx->lobby.selected_peer_idx;
        if (peer_idx >= 0 &&
            chess_lobby_get_challenge_state(&ctx->lobby, peer_idx) == CHESS_CHALLENGE_OUTGOING_PENDING &&
            !chess_lobby_has_offer_been_sent(&ctx->lobby, peer_idx)) {
            ChessOfferPayload offer;
            memset(&offer, 0, sizeof(offer));
            SDL_strlcpy(offer.challenger_uuid, ctx->network_session.local_peer.uuid, sizeof(offer.challenger_uuid));
            if (chess_tcp_send_offer(&ctx->connection, &offer)) {
                chess_lobby_mark_offer_sent(&ctx->lobby, peer_idx);
                SDL_Log("Sent OFFER to selected peer");
            }
        }
    }
}

static void send_start_if_needed(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->network_session.state != CHESS_NET_IN_GAME ||
        !ctx->hello_completed ||
        !ctx->challenge_exchange_completed ||
        ctx->start_completed ||
        ctx->network_session.role != CHESS_ROLE_SERVER ||
        ctx->connection.fd < 0 ||
        ctx->start_sent) {
        return;
    }

    memset(&ctx->pending_start_payload, 0, sizeof(ctx->pending_start_payload));
    ctx->pending_start_payload.game_id = make_game_id(&ctx->network_session.local_peer, &ctx->network_session.remote_peer);
    ctx->pending_start_payload.assigned_color = CHESS_COLOR_BLACK;
    ctx->pending_start_payload.initial_turn = CHESS_COLOR_WHITE;
    SDL_strlcpy(ctx->pending_start_payload.white_uuid, ctx->network_session.local_peer.uuid, sizeof(ctx->pending_start_payload.white_uuid));
    SDL_strlcpy(ctx->pending_start_payload.black_uuid, ctx->network_session.remote_peer.uuid, sizeof(ctx->pending_start_payload.black_uuid));

    if (chess_tcp_send_start(&ctx->connection, &ctx->pending_start_payload)) {
        ctx->start_sent = true;
    } else {
        ctx->start_failures += 1u;
        if (ctx->start_failures == 1u || (ctx->start_failures % 5u) == 0u) {
            SDL_Log("START send failed (%u failures), will retry", ctx->start_failures);
        }
    }
}

static void tick_network(AppLoopContext *ctx)
{
    ChessSocketEvents connection_phase_events;

    if (!ctx) {
        return;
    }

    poll_socket_events(&ctx->listener, &ctx->connection, &connection_phase_events);
    advance_transport_connection(ctx, &connection_phase_events);
    advance_hello_handshake(ctx);
    send_pending_offer_if_needed(ctx);

    drain_incoming_packets(ctx);
    send_start_if_needed(ctx);
}

int app_run(void)
{
    AppLoopContext ctx;

    app_loop_context_init_defaults(&ctx);

    if (!init_app_window_and_renderer(&ctx)) {
        return 1;
    }

    if (!init_app_networking(&ctx)) {
        app_loop_context_shutdown(&ctx);
        return 1;
    }

    while (ctx.running) {
        handle_sdl_events(&ctx);
        poll_discovery_and_update_lobby(&ctx);
        tick_network(&ctx);

        chess_network_session_step(&ctx.network_session);

        log_network_state_transition(&ctx);

        render_frame(&ctx);
    }

    app_loop_context_shutdown(&ctx);
    return 0;
}
