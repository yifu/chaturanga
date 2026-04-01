#include "game_internal.h"
#include "chess_app/ui_lobby.h"
#include "chess_app/render_board.h"

#include <SDL3_ttf/SDL_ttf.h>

/* ------------------------------------------------------------------ */
/*  Shared module-level state (owned here, extern in game_internal.h)  */
/* ------------------------------------------------------------------ */

SDL_FRect s_lobby_button_rect = {0};
bool      s_lobby_button_visible = false;
float     s_cap_cursor_start_top = 0.0f;
float     s_cap_cursor_start_bottom = 0.0f;

/* ------------------------------------------------------------------ */
/*  Public: board width                                                */
/* ------------------------------------------------------------------ */

int chess_ui_board_width_for_window(int window_width, bool game_started)
{
    int board_width;

    if (!game_started) {
        return window_width;
    }

    board_width = window_width - CHESS_UI_HISTORY_PANEL_WIDTH;
    if (board_width <= 0) {
        return window_width;
    }
    return board_width;
}

/* ------------------------------------------------------------------ */
/*  Public: screen to board conversion                                 */
/* ------------------------------------------------------------------ */

bool chess_ui_screen_to_board_square(
    AppContext *ctx, int mouse_x, int mouse_y, int *out_file, int *out_rank)
{
    int width = 0;
    int board_width = 0;
    int height = 0;
    int board_y = 0;
    int board_height;
    int local_mouse_y;
    float cell_w;
    float cell_h;
    bool black_perspective;
    int screen_file;
    int screen_rank;

    if (!ctx || !ctx->win.window || !out_file || !out_rank) {
        return false;
    }

    SDL_GetWindowSize(ctx->win.window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network.network_session.game_started);
    if (mouse_x < 0 || mouse_x >= board_width) {
        return false;
    }

    if (ctx->network.network_session.game_started) {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
    } else {
        board_height = height;
    }
    if (board_height <= 0) {
        return false;
    }

    local_mouse_y = mouse_y - board_y;
    if (local_mouse_y < 0 || local_mouse_y >= board_height) {
        return false;
    }

    cell_w = (float)board_width / (float)CHESS_BOARD_SIZE;
    cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    screen_file = (int)(mouse_x / cell_w);
    screen_rank = (int)(local_mouse_y / cell_h);
    *out_file = screen_to_board_index(screen_file, black_perspective);
    *out_rank = screen_to_board_index(screen_rank, black_perspective);

    if (*out_file < 0 || *out_file >= CHESS_BOARD_SIZE || *out_rank < 0 || *out_rank >= CHESS_BOARD_SIZE) {
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Promotion from mouse click                                         */
/* ------------------------------------------------------------------ */

uint8_t chess_ui_promotion_from_mouse(AppContext *ctx, int mouse_x, int mouse_y)
{
    const uint8_t choices[] = {
        CHESS_PROMOTION_QUEEN,
        CHESS_PROMOTION_ROOK,
        CHESS_PROMOTION_BISHOP,
        CHESS_PROMOTION_KNIGHT
    };
    int width;
    int board_width;
    int height;
    int board_y = 0;
    int board_height;
    size_t i;

    if (!ctx || !ctx->win.window || !ctx->ui.drag.promotion_pending) {
        return CHESS_PROMOTION_NONE;
    }

    SDL_GetWindowSize(ctx->win.window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network.network_session.game_started);
    if (ctx->network.network_session.game_started) {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
    } else {
        board_height = height;
    }
    for (i = 0; i < SDL_arraysize(choices); ++i) {
        SDL_FRect rect;
        if (chess_ui_promotion_choice_rect(ctx, board_width, board_y, board_height, choices[i], &rect) &&
            mouse_x >= (int)rect.x &&
            mouse_x < (int)(rect.x + rect.w) &&
            mouse_y >= (int)rect.y &&
            mouse_y < (int)(rect.y + rect.h)) {
            return choices[i];
        }
    }

    return CHESS_PROMOTION_NONE;
}

/* ------------------------------------------------------------------ */
/*  Public: full frame render                                          */
/* ------------------------------------------------------------------ */

void chess_ui_render_frame(AppContext *ctx)
{
    int width = 0;
    int board_width = 0;
    int height = 0;
    int board_y = 0;
    int board_height;
    bool hide_piece = false;
    int hidden_file = -1;
    int hidden_rank = -1;

    if (!ctx || !ctx->win.renderer || !ctx->win.window) {
        return;
    }

    SDL_GetWindowSize(ctx->win.window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network.network_session.game_started);
    SDL_SetRenderDrawColor(ctx->win.renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->win.renderer);

    if (!ctx->network.network_session.game_started) {
        board_height = height;
        /* Update lobby hover state and cursor */
        {
            float mx = 0.0f;
            float my = 0.0f;
            int hovered;
            SDL_GetMouseState(&mx, &my);
            hovered = chess_lobby_find_clicked_peer(ctx->win.window, &ctx->game.lobby, (int)mx, (int)my);
            ctx->game.lobby.hovered_peer_idx = hovered;
            if (hovered >= 0 && ctx->win.cursor_pointer) {
                SDL_SetCursor(ctx->win.cursor_pointer);
            } else if (ctx->win.cursor_default) {
                SDL_SetCursor(ctx->win.cursor_default);
            }
        }
        chess_lobby_render(ctx->win.renderer, width, height, &ctx->game.lobby, s_lobby_font ? s_lobby_font : s_coord_font);
    } else {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
        if (board_height < 0) {
            board_height = height;
            board_y = 0;
        }

        if (ctx->ui.drag.drag_active) {
            hide_piece = true;
            hidden_file = ctx->ui.drag.drag_from_file;
            hidden_rank = ctx->ui.drag.drag_from_rank;
        } else if (ctx->ui.snap_back_anim.active) {
            hide_piece = true;
            hidden_file = ctx->ui.snap_back_anim.to_file;
            hidden_rank = ctx->ui.snap_back_anim.to_rank;
        } else if (ctx->ui.remote_move_anim.active) {
            hide_piece = true;
            hidden_file = ctx->ui.remote_move_anim.to_file;
            hidden_rank = ctx->ui.remote_move_anim.to_rank;
        }

        chess_ui_render_player_panels(ctx, board_width, height, board_y, board_height);
        render_board(ctx->win.renderer, board_width, board_y, board_height);
        chess_ui_render_capture_animation(ctx, board_width, board_y, board_height);
        chess_ui_render_pending_capture_piece(ctx, board_width, board_y, board_height);
        chess_ui_render_game_overlay(
            ctx,
            ctx->win.renderer,
            board_width,
            board_y,
            board_height,
            &ctx->game.game_state,
            ctx->network.network_session.local_color,
            hide_piece,
            hidden_file,
            hidden_rank);
        chess_ui_render_promotion_overlay(ctx, board_width, board_y, board_height);
        chess_ui_render_remote_move_animation(ctx, board_width, board_y, board_height);
        chess_ui_render_snap_back_animation(ctx, board_width, board_y, board_height);
        chess_ui_render_drag_preview(ctx, board_width, board_height);
        chess_ui_render_board_coordinates(ctx->win.renderer, board_width, board_y, board_height, ctx->network.network_session.local_color);
        chess_ui_render_game_over_banner(ctx, board_width, board_y, board_height);
        chess_ui_render_move_history_panel(ctx, width, height, board_width);
    }

    chess_ui_render_status_message(ctx, board_width, board_y);

    SDL_RenderPresent(ctx->win.renderer);
}
