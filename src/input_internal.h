/**
 * Internal header shared by the split input_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/input_handler.c
 *   src/input_board.c
 */
#ifndef CHESS_APP_INPUT_INTERNAL_H
#define CHESS_APP_INPUT_INTERNAL_H

#include "chess_app/app_context.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Board interaction (defined in input_board.c)                       */
/* ------------------------------------------------------------------ */

bool chess_input_try_send_local_move(AppContext *ctx, int to_file, int to_rank, uint8_t promotion);
void chess_input_handle_board_mouse_down(AppContext *ctx, int mouse_x, int mouse_y);
void chess_input_handle_board_mouse_motion(AppContext *ctx, int mouse_x, int mouse_y);
void chess_input_handle_board_mouse_up(AppContext *ctx, int mouse_x, int mouse_y);

#endif /* CHESS_APP_INPUT_INTERNAL_H */
