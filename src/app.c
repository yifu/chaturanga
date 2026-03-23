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
static TTF_Font    *s_lobby_font                     = NULL;
static SDL_Texture *s_piece_textures[CHESS_PIECE_COUNT];
static SDL_Texture *s_file_label_textures[CHESS_BOARD_SIZE][2];
static SDL_Texture *s_rank_label_textures[CHESS_BOARD_SIZE][2];
static bool         s_ttf_initialized                = false;
static bool         s_lobby_icon_pending_available   = false;
static bool         s_lobby_icon_incoming_available  = false;
static bool         s_lobby_icon_matched_available   = false;
static const char  *s_lobby_font_path                = NULL;
static const uint32_t s_remote_move_anim_default_ms  = 160u;

static TTF_Font *open_font_from_candidates(const char * const *font_paths, float font_size)
{
    int i;

    for (i = 0; font_paths[i] != NULL; ++i) {
        TTF_Font *font = TTF_OpenFont(font_paths[i], font_size);
        if (font) {
            SDL_Log("UI: loaded font %s (size=%.1f)", font_paths[i], (double)font_size);
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

static bool text_surface_equals(SDL_Surface *a, SDL_Surface *b)
{
    int y;

    if (!a || !b) {
        return false;
    }

    if (a->w != b->w || a->h != b->h || a->pitch != b->pitch || a->format != b->format) {
        return false;
    }

    for (y = 0; y < a->h; ++y) {
        const uint8_t *row_a = (const uint8_t *)a->pixels + y * a->pitch;
        const uint8_t *row_b = (const uint8_t *)b->pixels + y * b->pitch;
        if (memcmp(row_a, row_b, (size_t)a->pitch) != 0) {
            return false;
        }
    }

    return true;
}

static bool font_supports_icon(TTF_Font *font, const char *icon_utf8)
{
    const SDL_Color color = {255, 255, 255, 255};
    SDL_Surface *icon = NULL;
    SDL_Surface *tofu_square = NULL;
    SDL_Surface *question = NULL;
    bool supported = false;

    if (!font) {
        return false;
    }

    icon = TTF_RenderText_Blended(font, icon_utf8, SDL_strlen(icon_utf8), color);
    tofu_square = TTF_RenderText_Blended(font, "□", SDL_strlen("□"), color);
    question = TTF_RenderText_Blended(font, "?", SDL_strlen("?"), color);

    if (!icon || !tofu_square || !question) {
        goto cleanup;
    }

    if (text_surface_equals(icon, tofu_square) || text_surface_equals(icon, question)) {
        goto cleanup;
    }

    supported = true;

cleanup:
    if (icon) {
        SDL_DestroySurface(icon);
    }
    if (tofu_square) {
        SDL_DestroySurface(tofu_square);
    }
    if (question) {
        SDL_DestroySurface(question);
    }

    return supported;
}

static int lobby_icon_coverage_score(TTF_Font *font)
{
    int score = 0;

    if (!font) {
        return 0;
    }

    if (font_supports_icon(font, "⏳")) {
        score += 1;
    }
    if (font_supports_icon(font, "⚔")) {
        score += 1;
    }
    if (font_supports_icon(font, "✓")) {
        score += 1;
    }

    return score;
}

static TTF_Font *open_lobby_font_from_candidates(const char * const *font_paths, float font_size, const char **out_font_path)
{
    int i;
    int best_score = 0;
    TTF_Font *best_font = NULL;
    const char *best_path = NULL;

    if (out_font_path) {
        *out_font_path = NULL;
    }

    for (i = 0; font_paths[i] != NULL; ++i) {
        TTF_Font *font = TTF_OpenFont(font_paths[i], font_size);
        int score;
        if (!font) {
            continue;
        }

        score = lobby_icon_coverage_score(font);
        if (score > best_score) {
            if (best_font) {
                TTF_CloseFont(best_font);
            }
            best_font = font;
            best_path = font_paths[i];
            best_score = score;
            continue;
        }

        TTF_CloseFont(font);
    }

    if (best_font) {
        SDL_Log(
            "UI: loaded best lobby icon font %s (size=%.1f, coverage=%d/3)",
            best_path ? best_path : "(unknown)",
            (double)font_size,
            best_score
        );
        if (out_font_path) {
            *out_font_path = best_path;
        }
        return best_font;
    }

    return NULL;
}

static const char *lobby_state_suffix(ChessChallengeState state)
{
    switch (state) {
    case CHESS_CHALLENGE_NONE:
        return "";
    case CHESS_CHALLENGE_OUTGOING_PENDING:
        return s_lobby_icon_pending_available ? " [⏳]" : " [PENDING]";
    case CHESS_CHALLENGE_INCOMING_PENDING:
        return s_lobby_icon_incoming_available ? " [⚔]" : " [INCOMING]";
    case CHESS_CHALLENGE_MATCHED:
        return s_lobby_icon_matched_available ? " [✓]" : " [MATCHED]";
    }

    return "";
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
    static const char * const chess_font_paths[] = {
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
        NULL
    };
    /* Coordinate labels: keep broad compatibility with readable latin glyphs */
    static const char * const coord_font_paths[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
        NULL
    };
    /* Lobby status icons: prefer symbol-complete fonts first */
    static const char * const lobby_icon_font_paths[] = {
        "/System/Library/Fonts/Supplemental/STIXTwoMath.otf",
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/CJKSymbolsFallback.ttc",
        "/System/Library/Fonts/Symbol.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/Library/Fonts/NotoSansSymbols2-Regular.ttf",
        "/System/Library/Fonts/Supplemental/NotoSansSymbols2-Regular.ttf",
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
        SDL_Log("UI: TTF_Init failed: %s", SDL_GetError());
        return;
    }
    s_ttf_initialized = true;

    s_chess_font = open_font_from_candidates(chess_font_paths, font_size);
    if (!s_chess_font) {
        SDL_Log("UI: no chess font found, piece rendering will use fallback rectangles");
    }

    if (s_chess_font) {
        for (i = 1; i < CHESS_PIECE_COUNT; ++i) {
            bool is_white = (i < (int)CHESS_PIECE_BLACK_PAWN);
            SDL_Color fg      = is_white ? white_fg      : black_fg;
            SDL_Color outline = is_white ? white_outline : black_outline;
            s_piece_textures[i] = make_outlined_glyph_texture(
                renderer, s_chess_font, piece_codepoints[i], fg, outline, 2);
            if (!s_piece_textures[i]) {
                SDL_Log("UI: failed to create texture for piece %d: %s", i, SDL_GetError());
            }
        }
    }

    s_coord_font = open_font_from_candidates(coord_font_paths, 16.0f);
    if (!s_coord_font) {
        SDL_Log("UI: no coordinate font found, board coordinates disabled");
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

    s_lobby_font = open_lobby_font_from_candidates(lobby_icon_font_paths, 16.0f, &s_lobby_font_path);
    if (s_lobby_font) {
        s_lobby_icon_pending_available = font_supports_icon(s_lobby_font, "⏳");
        s_lobby_icon_incoming_available = font_supports_icon(s_lobby_font, "⚔");
        s_lobby_icon_matched_available = font_supports_icon(s_lobby_font, "✓");
        SDL_Log(
            "UI: lobby rendering font %s (icons: pending=%s incoming=%s matched=%s)",
            s_lobby_font_path ? s_lobby_font_path : "(unknown)",
            s_lobby_icon_pending_available ? "yes" : "no",
            s_lobby_icon_incoming_available ? "yes" : "no",
            s_lobby_icon_matched_available ? "yes" : "no"
        );
    } else {
        s_lobby_icon_pending_available = false;
        s_lobby_icon_incoming_available = false;
        s_lobby_icon_matched_available = false;
        s_lobby_font = s_coord_font;
        s_lobby_font_path = "(fallback: coordinate font)";
        SDL_Log("UI: no icon-capable lobby font found, using ASCII fallback labels");
        SDL_Log("UI: lobby rendering font %s", s_lobby_font_path);
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
    if (s_lobby_font && s_lobby_font != s_coord_font) {
        TTF_CloseFont(s_lobby_font);
        s_lobby_font = NULL;
    }
    s_lobby_icon_pending_available = false;
    s_lobby_icon_incoming_available = false;
    s_lobby_icon_matched_available = false;
    s_lobby_font_path = NULL;
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
    ChessPlayerColor local_color,
    bool hide_piece,
    int hidden_file,
    int hidden_rank)
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

            if (hide_piece && file == hidden_file && rank == hidden_rank) {
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

    if (!chess_peer_init_local_identity(local_peer)) {
        SDL_Log("NET: could not initialize local peer identity");
        return false;
    }

        SDL_Log("NET: local peer initialized (uuid=%s display=%s@%s)",
            local_peer->uuid,
            local_peer->username,
            local_peer->hostname);
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

        /* Draw peer label and challenge state */
        {
            const char *challenge_icon = lobby_state_suffix(peer_state->challenge_state);
            SDL_Texture *name_tex = NULL;
            SDL_Texture *host_tex = NULL;
            SDL_Texture *status_tex = NULL;
            float name_w = 0.0f;
            float name_h = 0.0f;
            float host_w = 0.0f;
            float host_h = 0.0f;
            float status_w = 0.0f;
            float status_h = 0.0f;
            float cursor_x = peer_rect.x + 15.0f;
            float text_y;
            float max_h = 0.0f;
            const SDL_Color host_color = (SDL_Color){170, 170, 170, 255};

            if (peer_state->peer.username[0] != '\0' && peer_state->peer.hostname[0] != '\0') {
                char host_label[CHESS_PEER_HOSTNAME_MAX_LEN + 2];
                SDL_snprintf(host_label, sizeof(host_label), "@%s", peer_state->peer.hostname);
                name_tex = make_text_texture(renderer, font, peer_state->peer.username, text_color);
                host_tex = make_text_texture(renderer, font, host_label, host_color);
            } else {
                char uuid_label[16];
                SDL_snprintf(uuid_label, sizeof(uuid_label), "%.8s...", peer_state->peer.uuid);
                name_tex = make_text_texture(renderer, font, uuid_label, text_color);
            }

            status_tex = make_text_texture(renderer, font, challenge_icon, text_color);

            if (name_tex) {
                SDL_GetTextureSize(name_tex, &name_w, &name_h);
                if (name_h > max_h) {
                    max_h = name_h;
                }
            }
            if (host_tex) {
                SDL_GetTextureSize(host_tex, &host_w, &host_h);
                if (host_h > max_h) {
                    max_h = host_h;
                }
            }
            if (status_tex) {
                SDL_GetTextureSize(status_tex, &status_w, &status_h);
                if (status_h > max_h) {
                    max_h = status_h;
                }
            }
            if (max_h <= 0.0f) {
                max_h = 18.0f;
            }

            text_y = peer_rect.y + (peer_rect.h - max_h) / 2.0f;

            if (name_tex) {
                SDL_FRect dst = {cursor_x, text_y + (max_h - name_h) / 2.0f, name_w, name_h};
                /* Slightly thicken username to approximate bold emphasis. */
                SDL_RenderTexture(renderer, name_tex, NULL, &dst);
                dst.x += 1.0f;
                SDL_RenderTexture(renderer, name_tex, NULL, &dst);
                cursor_x += name_w + 2.0f;
            }

            if (host_tex) {
                SDL_FRect dst = {cursor_x, text_y + (max_h - host_h) / 2.0f, host_w, host_h};
                SDL_RenderTexture(renderer, host_tex, NULL, &dst);
                cursor_x += host_w + 10.0f;
            } else {
                cursor_x += 10.0f;
            }

            if (status_tex) {
                SDL_FRect dst = {cursor_x, text_y + (max_h - status_h) / 2.0f, status_w, status_h};
                SDL_RenderTexture(renderer, status_tex, NULL, &dst);
            }

            if (status_tex) { SDL_DestroyTexture(status_tex); }
            if (host_tex) { SDL_DestroyTexture(host_tex); }
            if (name_tex) { SDL_DestroyTexture(name_tex); }
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
    bool drag_active;
    ChessPiece drag_piece;
    int drag_from_file;
    int drag_from_rank;
    int drag_mouse_x;
    int drag_mouse_y;
    bool remote_move_anim_active;
    ChessPiece remote_move_anim_piece;
    int remote_move_from_file;
    int remote_move_from_rank;
    int remote_move_to_file;
    int remote_move_to_rank;
    uint64_t remote_move_anim_started_at_ms;
    uint32_t remote_move_anim_duration_ms;
    char status_message[192];
    uint64_t status_message_until_ms;
    bool running;
} AppLoopContext;

static void net_reset_transport_progress(AppLoopContext *ctx);
static void app_handle_peer_disconnect(AppLoopContext *ctx, const char *reason);

/* App context and startup helpers */
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

static bool app_init_window_and_renderer(AppLoopContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("APP: SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    ctx->window = SDL_CreateWindow("SDL3 Chess Board", ctx->window_size, ctx->window_size, 0);
    if (!ctx->window) {
        SDL_Log("APP: SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        SDL_Log("APP: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
        SDL_Quit();
        return false;
    }

    init_piece_textures(ctx->renderer);
    return true;
}

static void app_init_runtime_state(AppLoopContext *ctx)
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
    ctx->drag_active = false;
    ctx->drag_piece = CHESS_PIECE_EMPTY;
    ctx->drag_from_file = -1;
    ctx->drag_from_rank = -1;
    ctx->drag_mouse_x = 0;
    ctx->drag_mouse_y = 0;
    ctx->remote_move_anim_active = false;
    ctx->remote_move_anim_piece = CHESS_PIECE_EMPTY;
    ctx->remote_move_from_file = -1;
    ctx->remote_move_from_rank = -1;
    ctx->remote_move_to_file = -1;
    ctx->remote_move_to_rank = -1;
    ctx->remote_move_anim_started_at_ms = 0;
    ctx->remote_move_anim_duration_ms = s_remote_move_anim_default_ms;
    ctx->status_message[0] = '\0';
    ctx->status_message_until_ms = 0;

    net_reset_transport_progress(ctx);
    chess_game_state_init(&ctx->game_state);
    chess_lobby_init(&ctx->lobby);
}

static void app_set_status_message(AppLoopContext *ctx, const char *message, uint32_t duration_ms)
{
    if (!ctx || !message) {
        return;
    }

    SDL_strlcpy(ctx->status_message, message, sizeof(ctx->status_message));
    ctx->status_message_until_ms = SDL_GetTicks() + (uint64_t)duration_ms;
}

static void app_clear_challenges(AppLoopContext *ctx)
{
    int i;

    if (!ctx) {
        return;
    }

    for (i = 0; i < ctx->lobby.discovered_peer_count; ++i) {
        chess_lobby_set_challenge_state(&ctx->lobby, i, CHESS_CHALLENGE_NONE);
    }
}

static void app_handle_peer_disconnect(AppLoopContext *ctx, const char *reason)
{
    if (!ctx) {
        return;
    }

    if (reason && reason[0] != '\0') {
        SDL_Log("NET: peer disconnected (%s)", reason);
    } else {
        SDL_Log("NET: peer disconnected");
    }

    chess_tcp_connection_close(&ctx->connection);
    net_reset_transport_progress(ctx);

    ctx->start_completed = false;
    ctx->start_failures = 0u;
    ctx->pending_start_payload.game_id = 0u;
    ctx->network_session.transport_ready = false;
    ctx->network_session.game_started = false;
    ctx->network_session.role = CHESS_ROLE_UNKNOWN;
    ctx->network_session.state = CHESS_NET_IDLE_DISCOVERY;
    ctx->drag_active = false;
    ctx->drag_piece = CHESS_PIECE_EMPTY;
    ctx->remote_move_anim_active = false;
    ctx->remote_move_anim_piece = CHESS_PIECE_EMPTY;
    chess_game_clear_selection(&ctx->game_state);
    app_clear_challenges(ctx);

    app_set_status_message(
        ctx,
        "Opponent disconnected. Waiting for reconnection support; back in lobby.",
        5000u
    );
}

static bool app_init_networking(AppLoopContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!init_local_peer(&ctx->local_peer)) {
        return false;
    }

    if (!chess_tcp_listener_open(&ctx->listener, 0)) {
        SDL_Log("NET: could not create TCP listener on ephemeral port");
        return false;
    }

    SDL_Log("NET: listener ready on port %u", (unsigned int)ctx->listener.port);

    app_init_runtime_state(ctx);

    if (!chess_discovery_start(&ctx->discovery, &ctx->local_peer, ctx->listener.port)) {
        SDL_Log("NET: discovery start failed");
        return false;
    }

    chess_network_session_init(&ctx->network_session, &ctx->local_peer);
    ctx->last_state = ctx->network_session.state;
    return true;
}

/* App input helpers */
static int app_find_clicked_lobby_peer(AppLoopContext *ctx, int mouse_x, int mouse_y)
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

static void app_handle_lobby_click(AppLoopContext *ctx, int clicked_peer)
{
    if (!ctx || clicked_peer < 0 || clicked_peer >= ctx->lobby.discovered_peer_count) {
        return;
    }

    if (ctx->lobby.selected_peer_idx != clicked_peer) {
        ctx->lobby.selected_peer_idx = clicked_peer;
        SDL_Log("LOBBY: selected peer %d", clicked_peer);
        return;
    }

    {
        ChessChallengeState current_state = chess_lobby_get_challenge_state(&ctx->lobby, clicked_peer);

        if (current_state == CHESS_CHALLENGE_NONE) {
            chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_OUTGOING_PENDING);
            chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[clicked_peer].peer);
            SDL_Log("LOBBY: challenge sent to peer %d (%.8s...)", clicked_peer, ctx->lobby.discovered_peers[clicked_peer].peer.uuid);
        } else if (current_state == CHESS_CHALLENGE_OUTGOING_PENDING) {
            chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_NONE);
            SDL_Log("LOBBY: challenge cancelled for peer %d", clicked_peer);
        } else if (current_state == CHESS_CHALLENGE_INCOMING_PENDING) {
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_uuid, ctx->network_session.local_peer.uuid, sizeof(accept.acceptor_uuid));

            if (ctx->connection.fd >= 0 && chess_tcp_send_accept(&ctx->connection, &accept)) {
                ctx->challenge_exchange_completed = true;
                chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_MATCHED);
                chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[clicked_peer].peer);
                SDL_Log("LOBBY: accepted challenge from peer %d (%.8s...)", clicked_peer, ctx->lobby.discovered_peers[clicked_peer].peer.uuid);
                SDL_Log("NET: challenge exchange completed (local accept), waiting START/ACK");
            } else {
                SDL_Log("NET: cannot accept challenge yet, transport not ready");
            }
        }
    }
}

static bool app_screen_to_board_square(
    AppLoopContext *ctx,
    int mouse_x,
    int mouse_y,
    int *out_file,
    int *out_rank)
{
    int width = 0;
    int height = 0;
    float cell_w;
    float cell_h;
    bool black_perspective;
    int screen_file;
    int screen_rank;

    if (!ctx || !ctx->window || !out_file || !out_rank) {
        return false;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    cell_h = (float)height / (float)CHESS_BOARD_SIZE;
    black_perspective = use_black_perspective(ctx->network_session.local_color);
    screen_file = (int)(mouse_x / cell_w);
    screen_rank = (int)(mouse_y / cell_h);
    *out_file = screen_to_board_index(screen_file, black_perspective);
    *out_rank = screen_to_board_index(screen_rank, black_perspective);

    if (*out_file < 0 || *out_file >= CHESS_BOARD_SIZE || *out_rank < 0 || *out_rank >= CHESS_BOARD_SIZE) {
        return false;
    }

    return true;
}

static bool app_try_send_local_move(AppLoopContext *ctx, int to_file, int to_rank)
{
    ChessMovePayload move;

    if (!ctx || ctx->connection.fd < 0) {
        return false;
    }

    if (!chess_game_try_local_move(&ctx->game_state, ctx->network_session.local_color, to_file, to_rank, &move)) {
        return false;
    }

    if (!chess_tcp_send_packet(
            &ctx->connection,
            CHESS_MSG_MOVE,
            ctx->move_sequence++,
            &move,
            (uint32_t)sizeof(move))) {
        SDL_Log("NET: failed to send MOVE packet, closing connection");
        app_handle_peer_disconnect(ctx, "failed to send MOVE packet");
        return false;
    }

    SDL_Log(
        "GAME: sent local move (%u,%u) -> (%u,%u)",
        (unsigned)move.from_file,
        (unsigned)move.from_rank,
        (unsigned)move.to_file,
        (unsigned)move.to_rank
    );

    return true;
}

static void app_handle_board_mouse_down(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    int file;
    int rank;

    if (!ctx || ctx->connection.fd < 0) {
        return;
    }

    if (!app_screen_to_board_square(ctx, mouse_x, mouse_y, &file, &rank)) {
        return;
    }

    if (chess_game_select_local_piece(&ctx->game_state, ctx->network_session.local_color, file, rank)) {
        ctx->drag_active = true;
        ctx->drag_piece = chess_game_get_piece(&ctx->game_state, file, rank);
        ctx->drag_from_file = file;
        ctx->drag_from_rank = rank;
        ctx->drag_mouse_x = mouse_x;
        ctx->drag_mouse_y = mouse_y;
        return;
    }

    if (ctx->game_state.has_selection) {
        if (ctx->game_state.selected_file == file && ctx->game_state.selected_rank == rank) {
            chess_game_clear_selection(&ctx->game_state);
            return;
        }

        (void)app_try_send_local_move(ctx, file, rank);
    }
}

static void app_handle_board_mouse_motion(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    if (!ctx || !ctx->drag_active) {
        return;
    }

    ctx->drag_mouse_x = mouse_x;
    ctx->drag_mouse_y = mouse_y;
}

static void app_handle_board_mouse_up(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    int to_file;
    int to_rank;

    if (!ctx || !ctx->drag_active) {
        return;
    }

    ctx->drag_mouse_x = mouse_x;
    ctx->drag_mouse_y = mouse_y;

    if (app_screen_to_board_square(ctx, mouse_x, mouse_y, &to_file, &to_rank)) {
        (void)app_try_send_local_move(ctx, to_file, to_rank);
    }

    ctx->drag_active = false;
    ctx->drag_piece = CHESS_PIECE_EMPTY;
    ctx->drag_from_file = -1;
    ctx->drag_from_rank = -1;
}

static void app_handle_events(AppLoopContext *ctx)
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

        if (!ctx->network_session.game_started) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                ctx->lobby.discovered_peer_count > 0) {
                const int clicked_peer = app_find_clicked_lobby_peer(ctx, event.button.x, event.button.y);
                if (clicked_peer >= 0) {
                    app_handle_lobby_click(ctx, clicked_peer);
                }
            }
            continue;
        }

        if (ctx->connection.fd < 0) {
            continue;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            app_handle_board_mouse_down(ctx, event.button.x, event.button.y);
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            app_handle_board_mouse_motion(ctx, event.motion.x, event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            app_handle_board_mouse_up(ctx, event.button.x, event.button.y);
        }
    }
}

/* App frame helpers */
static void app_poll_discovery_and_update_lobby(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (!ctx->network_session.peer_available) {
        if (chess_discovery_poll(&ctx->discovery, &ctx->discovered_peer)) {
            chess_lobby_add_or_update_peer(&ctx->lobby, &ctx->discovered_peer.peer, ctx->discovered_peer.tcp_port);
            chess_network_session_set_remote(&ctx->network_session, &ctx->discovered_peer.peer);
            SDL_Log(
                "LOBBY: discovered peer %.8s... (port=%u)",
                ctx->discovered_peer.peer.uuid,
                (unsigned int)ctx->discovered_peer.tcp_port
            );
        }
    }
}

static void app_render_drag_preview(AppLoopContext *ctx, int width, int height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)height / (float)CHESS_BOARD_SIZE;

    if (!ctx || !ctx->renderer || !ctx->drag_active ||
        ctx->drag_piece <= CHESS_PIECE_EMPTY || ctx->drag_piece >= CHESS_PIECE_COUNT) {
        return;
    }

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->drag_piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = (float)ctx->drag_mouse_x - tex_w * 0.5f;
            dst.y = (float)ctx->drag_mouse_y - tex_h * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                (float)ctx->drag_mouse_x - cell_w * 0.25f,
                (float)ctx->drag_mouse_y - cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->drag_piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->renderer, &piece_rect);
        }
    }
}

static void app_update_remote_move_animation(AppLoopContext *ctx)
{
    uint64_t now;
    uint64_t elapsed;

    if (!ctx || !ctx->remote_move_anim_active) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->remote_move_anim_started_at_ms;
    if (ctx->remote_move_anim_duration_ms == 0u || elapsed >= (uint64_t)ctx->remote_move_anim_duration_ms) {
        ctx->remote_move_anim_active = false;
        ctx->remote_move_anim_piece = CHESS_PIECE_EMPTY;
        ctx->remote_move_from_file = -1;
        ctx->remote_move_from_rank = -1;
        ctx->remote_move_to_file = -1;
        ctx->remote_move_to_rank = -1;
    }
}

static void app_render_remote_move_animation(AppLoopContext *ctx, int width, int height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(ctx->network_session.local_color);
    uint64_t now;
    uint64_t elapsed;
    float t;
    int from_screen_file;
    int from_screen_rank;
    int to_screen_file;
    int to_screen_rank;
    float interp_file;
    float interp_rank;

    if (!ctx || !ctx->renderer || !ctx->remote_move_anim_active ||
        ctx->remote_move_anim_piece <= CHESS_PIECE_EMPTY ||
        ctx->remote_move_anim_piece >= CHESS_PIECE_COUNT) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->remote_move_anim_started_at_ms;
    if (ctx->remote_move_anim_duration_ms == 0u) {
        t = 1.0f;
    } else {
        t = (float)elapsed / (float)ctx->remote_move_anim_duration_ms;
        if (t > 1.0f) {
            t = 1.0f;
        }
    }

    from_screen_file = board_to_screen_index(ctx->remote_move_from_file, black_perspective);
    from_screen_rank = board_to_screen_index(ctx->remote_move_from_rank, black_perspective);
    to_screen_file = board_to_screen_index(ctx->remote_move_to_file, black_perspective);
    to_screen_rank = board_to_screen_index(ctx->remote_move_to_rank, black_perspective);
    interp_file = (1.0f - t) * (float)from_screen_file + t * (float)to_screen_file;
    interp_rank = (1.0f - t) * (float)from_screen_rank + t * (float)to_screen_rank;

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->remote_move_anim_piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = interp_file * cell_w + (cell_w - tex_w) * 0.5f;
            dst.y = interp_rank * cell_h + (cell_h - tex_h) * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                interp_file * cell_w + cell_w * 0.25f,
                interp_rank * cell_h + cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->remote_move_anim_piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->renderer, &piece_rect);
        }
    }
}

static void app_render_status_message(AppLoopContext *ctx, int width)
{
    SDL_Texture *msg_tex;

    if (!ctx || !ctx->renderer || !ctx->status_message[0]) {
        return;
    }

    if (SDL_GetTicks() >= ctx->status_message_until_ms) {
        ctx->status_message[0] = '\0';
        return;
    }

    msg_tex = make_text_texture(
        ctx->renderer,
        s_lobby_font ? s_lobby_font : s_coord_font,
        ctx->status_message,
        (SDL_Color){255, 240, 130, 255}
    );
    if (msg_tex) {
        float tex_w = 0.0f;
        float tex_h = 0.0f;
        SDL_FRect bg_rect;
        SDL_FRect dst;

        SDL_GetTextureSize(msg_tex, &tex_w, &tex_h);
        bg_rect.w = tex_w + 16.0f;
        bg_rect.h = tex_h + 10.0f;
        bg_rect.x = ((float)width - bg_rect.w) * 0.5f;
        bg_rect.y = 8.0f;
        dst.w = tex_w;
        dst.h = tex_h;
        dst.x = bg_rect.x + 8.0f;
        dst.y = bg_rect.y + 5.0f;

        SDL_SetRenderDrawColor(ctx->renderer, 35, 35, 35, 230);
        SDL_RenderFillRect(ctx->renderer, &bg_rect);
        SDL_SetRenderDrawColor(ctx->renderer, 215, 170, 70, 255);
        SDL_RenderRect(ctx->renderer, &bg_rect);
        SDL_RenderTexture(ctx->renderer, msg_tex, NULL, &dst);
        SDL_DestroyTexture(msg_tex);
    }
}

static void app_render_frame(AppLoopContext *ctx)
{
    int width = 0;
    int height = 0;
    bool hide_piece = false;
    int hidden_file = -1;
    int hidden_rank = -1;

    if (!ctx || !ctx->renderer || !ctx->window) {
        return;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->renderer);

    if (!ctx->network_session.game_started) {
        render_lobby(ctx->renderer, width, height, &ctx->lobby, s_lobby_font ? s_lobby_font : s_coord_font);
    } else {
        if (ctx->drag_active) {
            hide_piece = true;
            hidden_file = ctx->drag_from_file;
            hidden_rank = ctx->drag_from_rank;
        } else if (ctx->remote_move_anim_active) {
            hide_piece = true;
            hidden_file = ctx->remote_move_to_file;
            hidden_rank = ctx->remote_move_to_rank;
        }

        render_board(ctx->renderer, width, height);
        render_game_overlay(
            ctx->renderer,
            width,
            height,
            &ctx->game_state,
            ctx->network_session.local_color,
            hide_piece,
            hidden_file,
            hidden_rank);
        app_render_remote_move_animation(ctx, width, height);
        app_render_drag_preview(ctx, width, height);
        render_board_coordinates(ctx->renderer, width, height, ctx->network_session.local_color);
    }

    app_render_status_message(ctx, width);

    SDL_RenderPresent(ctx->renderer);
}

static void app_log_network_state_transition(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->network_session.state == ctx->last_state) {
        return;
    }

    SDL_Log(
        "NET: state changed %s -> %s",
        network_state_to_string(ctx->last_state),
        network_state_to_string(ctx->network_session.state)
    );

    if (ctx->network_session.state == CHESS_NET_CONNECTING) {
        if (ctx->network_session.role == CHESS_ROLE_SERVER) {
            SDL_Log("NET: local role SERVER (smaller IP)");
        } else if (ctx->network_session.role == CHESS_ROLE_CLIENT) {
            SDL_Log("NET: local role CLIENT");
        }
    }

    ctx->last_state = ctx->network_session.state;
}

/* Network tick helpers */
static void net_reset_transport_progress(AppLoopContext *ctx)
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

static bool net_receive_next_packet(AppLoopContext *ctx, ChessPacketHeader *header, uint8_t *payload, size_t payload_capacity)
{
    if (!ctx || !header || !payload) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(&ctx->connection, 1, header)) {
        SDL_Log("NET: failed to read packet header, closing connection");
        app_handle_peer_disconnect(ctx, "failed to read packet header");
        return false;
    }

    if (header->payload_size > payload_capacity) {
        SDL_Log("NET: received oversized payload (%u), closing connection", (unsigned)header->payload_size);
        app_handle_peer_disconnect(ctx, "received oversized payload");
        return false;
    }

    if (header->payload_size > 0u && !chess_tcp_recv_payload(&ctx->connection, 1, payload, header->payload_size)) {
        SDL_Log("NET: failed to read packet payload, closing connection");
        app_handle_peer_disconnect(ctx, "failed to read packet payload");
        return false;
    }

    return true;
}

static void net_handle_hello_packet(AppLoopContext *ctx, const ChessHelloPayload *hello)
{
    if (!ctx || !hello) {
        return;
    }

    ctx->hello_received = true;
    SDL_Log("NET: received HELLO from remote peer (%.8s...)", hello->uuid);
}

static void net_handle_offer_packet(AppLoopContext *ctx, const ChessOfferPayload *offer)
{
    int peer_idx = -1;
    int i;

    if (!ctx || !offer || ctx->challenge_exchange_completed) {
        return;
    }

    SDL_Log("NET: received OFFER from remote peer (%.8s...)", offer->challenger_uuid);

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

static void net_handle_accept_packet(AppLoopContext *ctx, const ChessAcceptPayload *accept)
{
    int peer_idx;
    int i;

    if (!ctx || !accept || ctx->challenge_exchange_completed) {
        return;
    }

    peer_idx = ctx->lobby.selected_peer_idx;
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

    SDL_Log("NET: received ACCEPT from remote peer (%.8s...)", accept->acceptor_uuid);
    SDL_Log("NET: challenge exchange completed (remote accept), waiting START/ACK");
}

static void net_handle_start_packet(AppLoopContext *ctx, const ChessStartPayload *start_payload)
{
    if (!ctx || !start_payload) {
        return;
    }

    if (ctx->network_session.role != CHESS_ROLE_CLIENT || ctx->start_completed) {
        return;
    }

    if (chess_tcp_send_ack(&ctx->connection, CHESS_MSG_START, 2u, 0u)) {
        chess_network_session_start_game(
            &ctx->network_session,
            start_payload->game_id,
            (ChessPlayerColor)start_payload->assigned_color
        );
        ctx->start_completed = true;
        chess_game_state_init(&ctx->game_state);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network_session.game_id,
            ctx->network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            start_payload->initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

static void net_handle_ack_packet(AppLoopContext *ctx, const ChessAckPayload *ack)
{
    if (!ctx || !ack || ctx->network_session.role != CHESS_ROLE_SERVER) {
        return;
    }

    if (ack->acked_message_type == CHESS_MSG_HELLO &&
        ack->acked_sequence == 1u &&
        ack->status_code == 0u) {
        ctx->hello_ack_received = true;
        return;
    }

    if (ctx->start_sent &&
        !ctx->start_completed &&
        ack->acked_message_type == CHESS_MSG_START &&
        ack->acked_sequence == 2u &&
        ack->status_code == 0u) {
        SDL_Log("NET: START ACK received, switching to game view");
        chess_network_session_start_game(&ctx->network_session, ctx->pending_start_payload.game_id,
            opposite_color((ChessPlayerColor)ctx->pending_start_payload.assigned_color));
        ctx->start_completed = true;
        chess_game_state_init(&ctx->game_state);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network_session.game_id,
            ctx->network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            ctx->pending_start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

static void net_handle_move_packet(AppLoopContext *ctx, const ChessMovePayload *move)
{
    ChessPiece moving_piece;
    ChessPlayerColor remote_color;

    if (!ctx || !move || !ctx->network_session.game_started) {
        return;
    }

    remote_color = opposite_color(ctx->network_session.local_color);
    if (remote_color == CHESS_COLOR_UNASSIGNED) {
        return;
    }

    moving_piece = chess_game_get_piece(&ctx->game_state, (int)move->from_file, (int)move->from_rank);

    if (chess_game_apply_remote_move(&ctx->game_state, remote_color, move)) {
        ChessPiece piece_to_animate = moving_piece;
        if (piece_to_animate == CHESS_PIECE_EMPTY) {
            piece_to_animate = chess_game_get_piece(&ctx->game_state, (int)move->to_file, (int)move->to_rank);
        }

        if (piece_to_animate != CHESS_PIECE_EMPTY) {
            ctx->remote_move_anim_active = true;
            ctx->remote_move_anim_piece = piece_to_animate;
            ctx->remote_move_from_file = (int)move->from_file;
            ctx->remote_move_from_rank = (int)move->from_rank;
            ctx->remote_move_to_file = (int)move->to_file;
            ctx->remote_move_to_rank = (int)move->to_rank;
            ctx->remote_move_anim_started_at_ms = SDL_GetTicks();
            if (ctx->remote_move_anim_duration_ms == 0u) {
                ctx->remote_move_anim_duration_ms = s_remote_move_anim_default_ms;
            }
        }

        SDL_Log(
            "GAME: applied remote move (%u,%u) -> (%u,%u)",
            (unsigned)move->from_file,
            (unsigned)move->from_rank,
            (unsigned)move->to_file,
            (unsigned)move->to_rank
        );
    } else {
        SDL_Log("GAME: ignoring invalid remote MOVE payload");
    }
}

static void net_dispatch_incoming_packet(AppLoopContext *ctx, const ChessPacketHeader *header, const uint8_t *payload)
{
    if (!ctx || !header || !payload) {
        return;
    }

    if (header->message_type == CHESS_MSG_HELLO && header->payload_size == sizeof(ChessHelloPayload)) {
        net_handle_hello_packet(ctx, (const ChessHelloPayload *)payload);
    } else if (header->message_type == CHESS_MSG_OFFER && header->payload_size == sizeof(ChessOfferPayload)) {
        net_handle_offer_packet(ctx, (const ChessOfferPayload *)payload);
    } else if (header->message_type == CHESS_MSG_ACCEPT && header->payload_size == sizeof(ChessAcceptPayload)) {
        net_handle_accept_packet(ctx, (const ChessAcceptPayload *)payload);
    } else if (header->message_type == CHESS_MSG_START && header->payload_size == sizeof(ChessStartPayload)) {
        net_handle_start_packet(ctx, (const ChessStartPayload *)payload);
    } else if (header->message_type == CHESS_MSG_ACK && header->payload_size == sizeof(ChessAckPayload)) {
        net_handle_ack_packet(ctx, (const ChessAckPayload *)payload);
    } else if (header->message_type == CHESS_MSG_MOVE && header->payload_size == sizeof(ChessMovePayload)) {
        net_handle_move_packet(ctx, (const ChessMovePayload *)payload);
    }
}

static void net_drain_incoming_packets(AppLoopContext *ctx)
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

        if (!net_receive_next_packet(ctx, &header, payload, sizeof(payload))) {
            break;
        }

        net_dispatch_incoming_packet(ctx, &header, payload);
    }
}

static void net_advance_transport_connection(AppLoopContext *ctx, const ChessSocketEvents *socket_events)
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
                SDL_Log("NET: accepted TCP client connection");
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
                SDL_Log("NET: connected to remote TCP host");
            }
        }
    }
}

static void net_advance_hello_handshake(AppLoopContext *ctx)
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
            SDL_Log("NET: HELLO handshake completed (client)");
        } else if (ctx->network_session.role == CHESS_ROLE_SERVER &&
                   ctx->hello_received &&
                   ctx->hello_sent &&
                   ctx->hello_ack_received) {
            ctx->hello_completed = true;
            chess_network_session_set_transport_ready(&ctx->network_session, true);
            SDL_Log("NET: HELLO handshake completed (server)");
        }
    }
}

static void net_send_pending_offer_if_needed(AppLoopContext *ctx)
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
                SDL_Log("NET: sent OFFER to selected peer");
            }
        }
    }
}

static void net_send_start_if_needed(AppLoopContext *ctx)
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
    ctx->pending_start_payload.initial_turn = CHESS_COLOR_WHITE;

    {
        const bool server_is_white = (arc4random() % 2u) == 0u;
        if (server_is_white) {
            ctx->pending_start_payload.assigned_color = CHESS_COLOR_BLACK;
            SDL_strlcpy(ctx->pending_start_payload.white_uuid, ctx->network_session.local_peer.uuid, sizeof(ctx->pending_start_payload.white_uuid));
            SDL_strlcpy(ctx->pending_start_payload.black_uuid, ctx->network_session.remote_peer.uuid, sizeof(ctx->pending_start_payload.black_uuid));
        } else {
            ctx->pending_start_payload.assigned_color = CHESS_COLOR_WHITE;
            SDL_strlcpy(ctx->pending_start_payload.white_uuid, ctx->network_session.remote_peer.uuid, sizeof(ctx->pending_start_payload.white_uuid));
            SDL_strlcpy(ctx->pending_start_payload.black_uuid, ctx->network_session.local_peer.uuid, sizeof(ctx->pending_start_payload.black_uuid));
        }
        SDL_Log("NET: color assignment — server=%s, client=%s",
            server_is_white ? "WHITE" : "BLACK",
            server_is_white ? "BLACK" : "WHITE");
    }

    if (chess_tcp_send_start(&ctx->connection, &ctx->pending_start_payload)) {
        ctx->start_sent = true;
    } else {
        ctx->start_failures += 1u;
        if (ctx->start_failures == 1u || (ctx->start_failures % 5u) == 0u) {
            SDL_Log("NET: START send failed (%u failures), will retry", ctx->start_failures);
        }
    }
}

static void app_tick_network(AppLoopContext *ctx)
{
    ChessSocketEvents connection_phase_events;

    if (!ctx) {
        return;
    }

    poll_socket_events(&ctx->listener, &ctx->connection, &connection_phase_events);
    net_advance_transport_connection(ctx, &connection_phase_events);
    net_advance_hello_handshake(ctx);
    net_send_pending_offer_if_needed(ctx);

    net_drain_incoming_packets(ctx);
    net_send_start_if_needed(ctx);
}

int app_run(void)
{
    AppLoopContext ctx;

    app_loop_context_init_defaults(&ctx);

    if (!app_init_window_and_renderer(&ctx)) {
        return 1;
    }

    if (!app_init_networking(&ctx)) {
        app_loop_context_shutdown(&ctx);
        return 1;
    }

    while (ctx.running) {
        app_handle_events(&ctx);
        app_poll_discovery_and_update_lobby(&ctx);
        app_tick_network(&ctx);

        chess_network_session_step(&ctx.network_session);

        app_log_network_state_transition(&ctx);
        app_update_remote_move_animation(&ctx);

        app_render_frame(&ctx);
    }

    app_loop_context_shutdown(&ctx);
    return 0;
}
