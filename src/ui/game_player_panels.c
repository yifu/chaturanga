/**
 * Game UI — player info panels (top / bottom of the board).
 *
 * Extracted from ui/game_panels.c.
 * Contains render_one_player_panel() and chess_ui_render_player_panels().
 */
#include "game_internal.h"

#include <SDL3_ttf/SDL_ttf.h>

/* ------------------------------------------------------------------ */
/*  Player info panels (top / bottom)                                  */
/* ------------------------------------------------------------------ */

static void render_one_player_panel(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const ChessPeerInfo *peer,
    const ChessCapturedPieces *captured,
    SDL_FRect panel_rect,
    ChessPlayerColor player_color,
    uint32_t remaining_ms,
    bool is_active_player)
{
    const SDL_Color name_color = {240, 240, 240, 255};
    const SDL_Color host_color = {170, 170, 170, 255};
    float cursor_x = panel_rect.x + 8.0f;
    float text_y;
    /* Piece types to show as captures, in display order. */
    const ChessPiece white_order[] = {
        CHESS_PIECE_WHITE_PAWN, CHESS_PIECE_WHITE_KNIGHT,
        CHESS_PIECE_WHITE_BISHOP, CHESS_PIECE_WHITE_ROOK,
        CHESS_PIECE_WHITE_QUEEN
    };
    const ChessPiece black_order[] = {
        CHESS_PIECE_BLACK_PAWN, CHESS_PIECE_BLACK_KNIGHT,
        CHESS_PIECE_BLACK_BISHOP, CHESS_PIECE_BLACK_ROOK,
        CHESS_PIECE_BLACK_QUEEN
    };
    const ChessPiece *cap_order;
    size_t cap_count;
    size_t ci;

    if (!renderer || !font) {
        return;
    }

    /* Background */
    SDL_SetRenderDrawColor(renderer, 28, 28, 34, 255);
    SDL_RenderFillRect(renderer, &panel_rect);

    /* Separator line at the edge closest to the board */
    {
        SDL_FRect sep;
        sep.x = panel_rect.x;
        sep.y = panel_rect.y + panel_rect.h - 1.0f;
        sep.w = panel_rect.w;
        sep.h = 1.0f;
        if (panel_rect.y < 1.0f) {
            /* Top panel: separator at bottom */
            sep.y = panel_rect.y + panel_rect.h - 1.0f;
        } else {
            /* Bottom panel: separator at top */
            sep.y = panel_rect.y;
        }
        SDL_SetRenderDrawColor(renderer, 55, 55, 65, 255);
        SDL_RenderFillRect(renderer, &sep);
    }

    /* Player name: username bold + @hostname gray */
    if (peer && peer->username[0] != '\0') {
        SDL_Texture *name_tex = make_text_texture(renderer, font, peer->username, name_color);
        if (name_tex) {
            float nw = 0.0f;
            float nh = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(name_tex, &nw, &nh);
            text_y = panel_rect.y + (panel_rect.h - nh) * 0.5f;
            dst.x = cursor_x;
            dst.y = text_y;
            dst.w = nw;
            dst.h = nh;
            /* Bold trick: render twice with 1px offset */
            SDL_RenderTexture(renderer, name_tex, NULL, &dst);
            dst.x += 1.0f;
            SDL_RenderTexture(renderer, name_tex, NULL, &dst);
            cursor_x += nw + 2.0f;
            SDL_DestroyTexture(name_tex);
        }

        if (peer->hostname[0] != '\0') {
            char host_label[CHESS_PEER_HOSTNAME_MAX_LEN + 2];
            SDL_Texture *host_tex;
            SDL_snprintf(host_label, sizeof(host_label), "@%s", peer->hostname);
            host_tex = make_text_texture(renderer, font, host_label, host_color);
            if (host_tex) {
                float hw = 0.0f;
                float hh = 0.0f;
                SDL_FRect dst;
                SDL_GetTextureSize(host_tex, &hw, &hh);
                dst.x = cursor_x;
                dst.y = panel_rect.y + (panel_rect.h - hh) * 0.5f;
                dst.w = hw;
                dst.h = hh;
                SDL_RenderTexture(renderer, host_tex, NULL, &dst);
                cursor_x += hw;
                SDL_DestroyTexture(host_tex);
            }
        }
    } else if (peer && peer->profile_id[0] != '\0') {
        char id_label[16];
        SDL_Texture *id_tex;
        SDL_snprintf(id_label, sizeof(id_label), "%.8s...", peer->profile_id);
        id_tex = make_text_texture(renderer, font, id_label, name_color);
        if (id_tex) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(id_tex, &tw, &th);
            text_y = panel_rect.y + (panel_rect.h - th) * 0.5f;
            dst.x = cursor_x;
            dst.y = text_y;
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(renderer, id_tex, NULL, &dst);
            cursor_x += tw;
            SDL_DestroyTexture(id_tex);
        }
    }

    /* Captured pieces: pieces captured BY this player (i.e. opponent's pieces) */
    if (!captured) {
        return;
    }

    /* This panel shows what opponent pieces this player has captured.
     * Player is white → show captured black pieces.
     * Player is black → show captured white pieces. */
    if (player_color == CHESS_COLOR_WHITE) {
        cap_order = black_order;
        cap_count = SDL_arraysize(black_order);
    } else {
        cap_order = white_order;
        cap_count = SDL_arraysize(white_order);
    }

    cursor_x += 12.0f;

    /* Record cursor origin for capture animation targeting */
    if (panel_rect.y < 1.0f) {
        s_cap_cursor_start_top = cursor_x;
    } else {
        s_cap_cursor_start_bottom = cursor_x;
    }

    for (ci = 0; ci < cap_count; ++ci) {
        int piece_idx = (int)cap_order[ci];
        uint8_t n = captured->count[piece_idx];
        uint8_t k;
        if (n == 0) {
            continue;
        }
        for (k = 0; k < n; ++k) {
            SDL_Texture *tex = s_piece_textures[piece_idx];
            if (tex) {
                float tex_w = 0.0f;
                float tex_h = 0.0f;
                float scale;
                SDL_FRect dst;
                SDL_GetTextureSize(tex, &tex_w, &tex_h);
                if (tex_h > 0.0f) {
                    scale = 24.0f / tex_h;
                } else {
                    scale = 1.0f;
                }
                dst.w = tex_w * scale;
                dst.h = 24.0f;
                dst.x = cursor_x;
                dst.y = panel_rect.y + (panel_rect.h - dst.h) * 0.5f;
                /* White silhouette halo behind dark pieces on dark panel */
                if (piece_idx >= (int)CHESS_PIECE_BLACK_PAWN &&
                    piece_idx <= (int)CHESS_PIECE_BLACK_KING &&
                    s_piece_silhouettes[piece_idx]) {
                    static const float offsets[][2] = {
                        {-2, -2}, {-1, -2}, { 0, -2}, { 1, -2}, { 2, -2},
                        {-2, -1},                               { 2, -1},
                        {-2,  0},                               { 2,  0},
                        {-2,  1},                               { 2,  1},
                        {-2,  2}, {-1,  2}, { 0,  2}, { 1,  2}, { 2,  2},
                    };
                    size_t oi;
                    for (oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); ++oi) {
                        SDL_FRect halo = dst;
                        halo.x += offsets[oi][0];
                        halo.y += offsets[oi][1];
                        SDL_RenderTexture(renderer, s_piece_silhouettes[piece_idx], NULL, &halo);
                    }
                }
                SDL_RenderTexture(renderer, tex, NULL, &dst);
                cursor_x += dst.w * 0.55f; /* overlap for compact display */
            }
        }
        cursor_x += 4.0f; /* gap between piece types */
    }

    /* Chess clock — right-aligned */
    {
        char clock_buf[16];
        uint32_t total_seconds = remaining_ms / 1000u;
        uint32_t minutes = total_seconds / 60u;
        uint32_t seconds = total_seconds % 60u;
        SDL_Color clock_color;
        SDL_Texture *clock_tex;

        if (remaining_ms < 30000u) {
            clock_color = (SDL_Color){230, 60, 60, 255};     /* red warning */
        } else if (is_active_player) {
            clock_color = (SDL_Color){255, 255, 255, 255};   /* bright white */
        } else {
            clock_color = (SDL_Color){160, 160, 160, 255};   /* dimmed */
        }

        SDL_snprintf(clock_buf, sizeof(clock_buf), "%02u:%02u", minutes, seconds);
        clock_tex = make_text_texture(renderer, font, clock_buf, clock_color);
        if (clock_tex) {
            float cw = 0.0f;
            float ch = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(clock_tex, &cw, &ch);
            dst.w = cw;
            dst.h = ch;
            dst.x = panel_rect.x + panel_rect.w - cw - 8.0f;
            dst.y = panel_rect.y + (panel_rect.h - ch) * 0.5f;
            /* Bold trick for active player */
            SDL_RenderTexture(renderer, clock_tex, NULL, &dst);
            if (is_active_player) {
                dst.x += 1.0f;
                SDL_RenderTexture(renderer, clock_tex, NULL, &dst);
            }
            SDL_DestroyTexture(clock_tex);
        }
    }
}

void chess_ui_render_player_panels(
    AppContext *ctx,
    int board_width,
    int window_height,
    int board_y,
    int board_height)
{
    const bool black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    const ChessPeerInfo *top_peer;
    const ChessPeerInfo *bottom_peer;
    ChessPlayerColor top_color;
    ChessPlayerColor bottom_color;
    ChessCapturedPieces captured;
    TTF_Font *font = s_coord_font;
    SDL_FRect top_rect;
    SDL_FRect bottom_rect;
    uint32_t white_display_ms;
    uint32_t black_display_ms;
    uint32_t top_clock_ms;
    uint32_t bottom_clock_ms;
    bool top_active;
    bool bottom_active;

    if (!ctx || !ctx->win.renderer) {
        return;
    }

    chess_game_compute_captured(&ctx->game.game_state, &captured);

    /* Compute displayed clock values */
    white_display_ms = ctx->game.white_remaining_ms;
    black_display_ms = ctx->game.black_remaining_ms;
    if (ctx->game.game_state.outcome == CHESS_OUTCOME_NONE &&
        ctx->network.network_session.game_started &&
        ctx->game.last_clock_sync_ticks > 0) {
        uint64_t elapsed = SDL_GetTicks() - ctx->game.last_clock_sync_ticks;
        if (ctx->game.game_state.side_to_move == CHESS_COLOR_WHITE) {
            white_display_ms = (elapsed >= white_display_ms) ? 0 : white_display_ms - (uint32_t)elapsed;
        } else {
            black_display_ms = (elapsed >= black_display_ms) ? 0 : black_display_ms - (uint32_t)elapsed;
        }
    }

    if (black_perspective) {
        /* Black at bottom (local), white at top (opponent) */
        top_peer = &ctx->network.network_session.remote_peer;
        top_color = CHESS_COLOR_WHITE;
        bottom_peer = &ctx->network.network_session.local_peer;
        bottom_color = CHESS_COLOR_BLACK;
        top_clock_ms = white_display_ms;
        bottom_clock_ms = black_display_ms;
    } else {
        /* White at bottom (local), black at top (opponent) */
        top_peer = &ctx->network.network_session.remote_peer;
        top_color = CHESS_COLOR_BLACK;
        bottom_peer = &ctx->network.network_session.local_peer;
        bottom_color = CHESS_COLOR_WHITE;
        top_clock_ms = black_display_ms;
        bottom_clock_ms = white_display_ms;
    }

    top_active = (ctx->game.game_state.side_to_move == top_color &&
                  ctx->game.game_state.outcome == CHESS_OUTCOME_NONE);
    bottom_active = (ctx->game.game_state.side_to_move == bottom_color &&
                     ctx->game.game_state.outcome == CHESS_OUTCOME_NONE);

    top_rect.x = 0.0f;
    top_rect.y = 0.0f;
    top_rect.w = (float)board_width;
    top_rect.h = (float)CHESS_UI_PLAYER_PANEL_HEIGHT;

    bottom_rect.x = 0.0f;
    bottom_rect.y = (float)(board_y + board_height);
    bottom_rect.w = (float)board_width;
    bottom_rect.h = (float)CHESS_UI_PLAYER_PANEL_HEIGHT;

    render_one_player_panel(ctx->win.renderer, font, top_peer, &captured, top_rect, top_color, top_clock_ms, top_active);
    render_one_player_panel(ctx->win.renderer, font, bottom_peer, &captured, bottom_rect, bottom_color, bottom_clock_ms, bottom_active);
}
