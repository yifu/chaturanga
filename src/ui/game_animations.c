/**
 * Game UI — move and capture animations.
 *
 * Extracted from ui/game.c to keep each module under ~800 lines.
 * Contains:
 *   - Remote-move animation (piece slides across the board)
 *   - Capture animation (piece flies to the player panel, shrinking)
 */
#include "game_internal.h"

/* ------------------------------------------------------------------ */
/*  Remote-move animation                                              */
/* ------------------------------------------------------------------ */

void chess_ui_update_remote_move_animation(AppContext *ctx)
{
    uint64_t now;
    uint64_t elapsed;

    if (!ctx || !ctx->ui.remote_move_anim.active) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->ui.remote_move_anim.started_at_ms;
    if (ctx->ui.remote_move_anim.duration_ms == 0u || elapsed >= (uint64_t)ctx->ui.remote_move_anim.duration_ms) {
        ctx->ui.remote_move_anim.active = false;
        ctx->ui.remote_move_anim.piece = CHESS_PIECE_EMPTY;
        ctx->ui.remote_move_anim.from_file = -1;
        ctx->ui.remote_move_anim.from_rank = -1;
        ctx->ui.remote_move_anim.to_file = -1;
        ctx->ui.remote_move_anim.to_rank = -1;

        /* Start deferred capture animation now that the slide is done */
        if (ctx->ui.capture_anim.pending) {
            ctx->ui.capture_anim.pending = false;
            chess_ui_start_capture_animation(
                ctx,
                ctx->ui.capture_anim.piece,
                ctx->ui.capture_anim.from_file,
                ctx->ui.capture_anim.from_rank);
        }
    }
}

void chess_ui_render_remote_move_animation(AppContext *ctx, int width, int board_y, int board_height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    uint64_t now;
    uint64_t elapsed;
    float t;
    int from_screen_file;
    int from_screen_rank;
    int to_screen_file;
    int to_screen_rank;
    float interp_file;
    float interp_rank;

    if (!ctx || !ctx->win.renderer || !ctx->ui.remote_move_anim.active ||
        ctx->ui.remote_move_anim.piece <= CHESS_PIECE_EMPTY ||
        ctx->ui.remote_move_anim.piece >= CHESS_PIECE_COUNT) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->ui.remote_move_anim.started_at_ms;
    if (ctx->ui.remote_move_anim.duration_ms == 0u) {
        t = 1.0f;
    } else {
        t = (float)elapsed / (float)ctx->ui.remote_move_anim.duration_ms;
        if (t > 1.0f) {
            t = 1.0f;
        }
    }

    from_screen_file = board_to_screen_index(ctx->ui.remote_move_anim.from_file, black_perspective);
    from_screen_rank = board_to_screen_index(ctx->ui.remote_move_anim.from_rank, black_perspective);
    to_screen_file = board_to_screen_index(ctx->ui.remote_move_anim.to_file, black_perspective);
    to_screen_rank = board_to_screen_index(ctx->ui.remote_move_anim.to_rank, black_perspective);
    interp_file = (1.0f - t) * (float)from_screen_file + t * (float)to_screen_file;
    interp_rank = (1.0f - t) * (float)from_screen_rank + t * (float)to_screen_rank;

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->ui.remote_move_anim.piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = interp_file * cell_w + (cell_w - tex_w) * 0.5f;
            dst.y = (float)board_y + interp_rank * cell_h + (cell_h - tex_h) * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->win.renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                interp_file * cell_w + cell_w * 0.25f,
                (float)board_y + interp_rank * cell_h + cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->ui.remote_move_anim.piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->win.renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->win.renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->win.renderer, &piece_rect);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Capture animation (piece flies to panel, shrinking)                */
/* ------------------------------------------------------------------ */

#define CHESS_CAPTURE_ANIM_DEFAULT_MS 350u

void chess_ui_update_capture_animation(AppContext *ctx)
{
    uint64_t now;
    uint64_t elapsed;

    if (!ctx || !ctx->ui.capture_anim.active) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->ui.capture_anim.started_at_ms;
    if (ctx->ui.capture_anim.duration_ms == 0u || elapsed >= (uint64_t)ctx->ui.capture_anim.duration_ms) {
        /* Animation finished: now add the piece to the captured list */
        if ((int)ctx->ui.capture_anim.piece > 0 &&
            (int)ctx->ui.capture_anim.piece < CHESS_PIECE_COUNT) {
            ctx->game.game_state.captured[(int)ctx->ui.capture_anim.piece]++;
        }
        ctx->ui.capture_anim.active = false;
        ctx->ui.capture_anim.piece = CHESS_PIECE_EMPTY;
    }
}

void chess_ui_start_capture_animation(
    AppContext *ctx,
    ChessPiece captured_piece,
    int from_file,
    int from_rank)
{
    bool captured_is_black;
    bool black_persp;

    if (!ctx || captured_piece == CHESS_PIECE_EMPTY || (int)captured_piece >= CHESS_PIECE_COUNT) {
        return;
    }

    captured_is_black = ((int)captured_piece >= (int)CHESS_PIECE_BLACK_PAWN);
    black_persp = use_black_perspective(ctx->network.network_session.local_color);

    ctx->ui.capture_anim.active = true;
    ctx->ui.capture_anim.piece = captured_piece;
    ctx->ui.capture_anim.from_file = from_file;
    ctx->ui.capture_anim.from_rank = from_rank;
    ctx->ui.capture_anim.target_top = (captured_is_black == black_persp);
    ctx->ui.capture_anim.started_at_ms = SDL_GetTicks();
    ctx->ui.capture_anim.duration_ms = CHESS_CAPTURE_ANIM_DEFAULT_MS;

    /* Hide the piece from the captured list during the animation;
       it will be re-added when the animation finishes. */
    if (ctx->game.game_state.captured[(int)captured_piece] > 0) {
        ctx->game.game_state.captured[(int)captured_piece]--;
    }
}

void chess_ui_render_pending_capture_piece(AppContext *ctx, int width, int board_y, int board_height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    int screen_file;
    int screen_rank;

    if (!ctx || !ctx->win.renderer || !ctx->ui.capture_anim.pending ||
        ctx->ui.capture_anim.piece <= CHESS_PIECE_EMPTY ||
        ctx->ui.capture_anim.piece >= CHESS_PIECE_COUNT) {
        return;
    }

    screen_file = board_to_screen_index(ctx->ui.capture_anim.from_file, black_perspective);
    screen_rank = board_to_screen_index(ctx->ui.capture_anim.from_rank, black_perspective);

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->ui.capture_anim.piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = (float)screen_file * cell_w + (cell_w - tex_w) * 0.5f;
            dst.y = (float)board_y + (float)screen_rank * cell_h + (cell_h - tex_h) * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->win.renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                (float)screen_file * cell_w + cell_w * 0.25f,
                (float)board_y + (float)screen_rank * cell_h + cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->ui.capture_anim.piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->win.renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->win.renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->win.renderer, &piece_rect);
        }
    }
}

void chess_ui_render_capture_animation(AppContext *ctx, int board_width, int board_y, int board_height)
{
    const float cell_w = (float)board_width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    const float panel_h = (float)CHESS_UI_PLAYER_PANEL_HEIGHT;
    const float target_size = 24.0f;
    uint64_t now;
    uint64_t elapsed;
    float t;
    int screen_file;
    int screen_rank;
    float start_x;
    float start_y;
    float start_size;
    float target_x;
    float target_y;
    float cur_x;
    float cur_y;
    float cur_size;
    SDL_Texture *tex;

    if (!ctx || !ctx->win.renderer || !ctx->ui.capture_anim.active ||
        ctx->ui.capture_anim.piece <= CHESS_PIECE_EMPTY ||
        ctx->ui.capture_anim.piece >= CHESS_PIECE_COUNT) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->ui.capture_anim.started_at_ms;
    if (ctx->ui.capture_anim.duration_ms == 0u) {
        t = 1.0f;
    } else {
        t = (float)elapsed / (float)ctx->ui.capture_anim.duration_ms;
        if (t > 1.0f) {
            t = 1.0f;
        }
    }

    /* Source position: center of the board square where capture happened */
    screen_file = board_to_screen_index(ctx->ui.capture_anim.from_file, black_perspective);
    screen_rank = board_to_screen_index(ctx->ui.capture_anim.from_rank, black_perspective);
    start_size = cell_h;
    start_x = screen_file * cell_w + cell_w * 0.5f;
    start_y = (float)board_y + screen_rank * cell_h + cell_h * 0.5f;

    /* Target position: compute exact final X by simulating the captured
       pieces layout.  The animated piece has been temporarily removed from
       captured[], so we replay the layout with it virtually re-inserted. */
    {
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
        bool piece_is_black = ((int)ctx->ui.capture_anim.piece >= (int)CHESS_PIECE_BLACK_PAWN);
        ChessPlayerColor panel_color;
        ChessCapturedPieces sim_cap;
        float sim_cursor;
        bool found = false;

        /* Determine which player panel captures this piece:
         * White captures black pieces, black captures white pieces. */
        panel_color = piece_is_black ? CHESS_COLOR_WHITE : CHESS_COLOR_BLACK;

        if (panel_color == CHESS_COLOR_WHITE) {
            cap_order = black_order;
            cap_count = SDL_arraysize(black_order);
        } else {
            cap_order = white_order;
            cap_count = SDL_arraysize(white_order);
        }

        /* Simulate with the animated piece included */
        chess_game_compute_captured(&ctx->game.game_state, &sim_cap);
        sim_cap.count[(int)ctx->ui.capture_anim.piece]++;

        sim_cursor = ctx->ui.capture_anim.target_top
            ? s_cap_cursor_start_top
            : s_cap_cursor_start_bottom;

        for (ci = 0; ci < cap_count && !found; ++ci) {
            int idx = (int)cap_order[ci];
            uint8_t n = sim_cap.count[idx];
            uint8_t k;
            if (n == 0) {
                continue;
            }
            for (k = 0; k < n; ++k) {
                SDL_Texture *ptex = s_piece_textures[idx];
                float pw = 0.0f;
                float ph = 0.0f;
                float ps;
                if (ptex) {
                    SDL_GetTextureSize(ptex, &pw, &ph);
                    ps = (ph > 0.0f) ? 24.0f / ph : 1.0f;
                    pw *= ps;
                }
                if (idx == (int)ctx->ui.capture_anim.piece && k == n - 1) {
                    /* This is the slot for the animated piece */
                    target_x = sim_cursor + pw * 0.5f;
                    found = true;
                    break;
                }
                if (ptex) {
                    sim_cursor += pw * 0.55f;
                }
            }
            if (!found) {
                sim_cursor += 4.0f;
            }
        }

        if (!found) {
            target_x = sim_cursor;
        }
    }
    if (ctx->ui.capture_anim.target_top) {
        target_y = panel_h * 0.5f;
    } else {
        target_y = (float)(board_y + board_height) + panel_h * 0.5f;
    }

    /* Linear interpolation of position and size */
    cur_x = (1.0f - t) * start_x + t * target_x;
    cur_y = (1.0f - t) * start_y + t * target_y;
    cur_size = (1.0f - t) * start_size + t * target_size;

    tex = s_piece_textures[(int)ctx->ui.capture_anim.piece];
    if (tex) {
        float tex_w = 0.0f;
        float tex_h = 0.0f;
        float scale;
        SDL_FRect dst;

        SDL_GetTextureSize(tex, &tex_w, &tex_h);
        scale = (tex_h > 0.0f) ? cur_size / tex_h : 1.0f;
        dst.w = tex_w * scale;
        dst.h = cur_size;
        dst.x = cur_x - dst.w * 0.5f;
        dst.y = cur_y - dst.h * 0.5f;

        /* White silhouette halo behind dark pieces */
        if ((int)ctx->ui.capture_anim.piece >= (int)CHESS_PIECE_BLACK_PAWN &&
            s_piece_silhouettes[(int)ctx->ui.capture_anim.piece]) {
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
                SDL_RenderTexture(ctx->win.renderer, s_piece_silhouettes[(int)ctx->ui.capture_anim.piece], NULL, &halo);
            }
        }

        SDL_RenderTexture(ctx->win.renderer, tex, NULL, &dst);
    }
}

/* ------------------------------------------------------------------ */
/*  Snap-back animation (piece returns to origin after illegal move)    */
/* ------------------------------------------------------------------ */

#define CHESS_SNAP_BACK_ANIM_DEFAULT_MS 150u

void chess_ui_update_snap_back_animation(AppContext *ctx)
{
    uint64_t now;
    uint64_t elapsed;

    if (!ctx || !ctx->ui.snap_back_anim.active) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->ui.snap_back_anim.started_at_ms;
    if (ctx->ui.snap_back_anim.duration_ms == 0u || elapsed >= (uint64_t)ctx->ui.snap_back_anim.duration_ms) {
        ctx->ui.snap_back_anim.active = false;
        ctx->ui.snap_back_anim.piece = CHESS_PIECE_EMPTY;
    }
}

void chess_ui_start_snap_back_animation(
    AppContext *ctx,
    ChessPiece piece,
    int to_file,
    int to_rank,
    float from_x,
    float from_y)
{
    if (!ctx || piece == CHESS_PIECE_EMPTY || (int)piece >= CHESS_PIECE_COUNT) {
        return;
    }

    ctx->ui.snap_back_anim.active = true;
    ctx->ui.snap_back_anim.piece = piece;
    ctx->ui.snap_back_anim.to_file = to_file;
    ctx->ui.snap_back_anim.to_rank = to_rank;
    ctx->ui.snap_back_anim.from_x = from_x;
    ctx->ui.snap_back_anim.from_y = from_y;
    ctx->ui.snap_back_anim.started_at_ms = SDL_GetTicks();
    ctx->ui.snap_back_anim.duration_ms = CHESS_SNAP_BACK_ANIM_DEFAULT_MS;
}

void chess_ui_render_snap_back_animation(AppContext *ctx, int width, int board_y, int board_height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    uint64_t now;
    uint64_t elapsed;
    float t;
    int screen_file;
    int screen_rank;
    float target_x;
    float target_y;
    float cur_x;
    float cur_y;

    if (!ctx || !ctx->win.renderer || !ctx->ui.snap_back_anim.active ||
        ctx->ui.snap_back_anim.piece <= CHESS_PIECE_EMPTY ||
        ctx->ui.snap_back_anim.piece >= CHESS_PIECE_COUNT) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->ui.snap_back_anim.started_at_ms;
    if (ctx->ui.snap_back_anim.duration_ms == 0u) {
        t = 1.0f;
    } else {
        t = (float)elapsed / (float)ctx->ui.snap_back_anim.duration_ms;
        if (t > 1.0f) {
            t = 1.0f;
        }
    }

    screen_file = board_to_screen_index(ctx->ui.snap_back_anim.to_file, black_perspective);
    screen_rank = board_to_screen_index(ctx->ui.snap_back_anim.to_rank, black_perspective);
    target_x = (float)screen_file * cell_w + cell_w * 0.5f;
    target_y = (float)board_y + (float)screen_rank * cell_h + cell_h * 0.5f;
    cur_x = (1.0f - t) * ctx->ui.snap_back_anim.from_x + t * target_x;
    cur_y = (1.0f - t) * ctx->ui.snap_back_anim.from_y + t * target_y;

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->ui.snap_back_anim.piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = cur_x - tex_w * 0.5f;
            dst.y = cur_y - tex_h * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->win.renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                cur_x - cell_w * 0.25f,
                cur_y - cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->ui.snap_back_anim.piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->win.renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->win.renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->win.renderer, &piece_rect);
        }
    }
}
