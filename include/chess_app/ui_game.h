#ifndef CHESS_APP_UI_GAME_H
#define CHESS_APP_UI_GAME_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "chess_app/app_context.h"
#include "chess_app/game_state.h"

#define CHESS_UI_HISTORY_PANEL_WIDTH 220
#define CHESS_UI_PLAYER_PANEL_HEIGHT 30

/**
 * Compute the board pixel width given the window width and game state.
 * Subtracts the move-history panel when the game is started.
 */
int chess_ui_board_width_for_window(int window_width, bool game_started);

/**
 * Convert screen pixel coordinates to board file/rank.
 * Returns true if the position is on the board.
 */
bool chess_ui_screen_to_board_square(
    AppContext *ctx, int mouse_x, int mouse_y, int *out_file, int *out_rank);

/**
 * Hit-test the promotion overlay. Returns the promotion choice at the mouse
 * position, or CHESS_PROMOTION_NONE.
 */
uint8_t chess_ui_promotion_from_mouse(AppContext *ctx, int mouse_x, int mouse_y);

/**
 * Buttons that may appear in the right panel.
 */
typedef enum ChessGameButton {
    CHESS_GAME_BUTTON_NONE = 0,
    CHESS_GAME_BUTTON_RESIGN,
    CHESS_GAME_BUTTON_DRAW,
    CHESS_GAME_BUTTON_ACCEPT_DRAW,
    CHESS_GAME_BUTTON_DECLINE_DRAW,
    CHESS_GAME_BUTTON_RETURN_LOBBY
} ChessGameButton;

/**
 * Hit-test the panel buttons. Returns which button (if any) was clicked.
 */
ChessGameButton chess_ui_game_button_from_mouse(AppContext *ctx, int mouse_x, int mouse_y);

/**
 * Advance the remote-move animation timer.
 * Call once per frame from the main loop.
 */
void chess_ui_update_remote_move_animation(AppContext *ctx);

/**
 * Full frame rendering: lobby, board, overlays, history panel, status.
 */
void chess_ui_render_frame(AppContext *ctx);

#endif /* CHESS_APP_UI_GAME_H */
