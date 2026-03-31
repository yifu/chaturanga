/**
 * Internal header shared by the split ui/game_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/ui/game.c
 *   src/ui/game_overlays.c
 *   src/ui/game_panels.c
 *   src/ui/game_player_panels.c
 *   src/ui/game_animations.c
 */
#ifndef CHESS_APP_UI_GAME_INTERNAL_H
#define CHESS_APP_UI_GAME_INTERNAL_H

#include "chess_app/ui_game.h"
#include "chess_app/ui_fonts.h"
#include "chess_app/network_session.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

/* ------------------------------------------------------------------ */
/*  Coordinate / perspective helpers (static inline)                   */
/* ------------------------------------------------------------------ */

static inline bool use_black_perspective(ChessPlayerColor local_color)
{
    return local_color == CHESS_COLOR_BLACK;
}

static inline int board_to_screen_index(int idx, bool black_perspective)
{
    return black_perspective ? (CHESS_BOARD_SIZE - 1 - idx) : idx;
}

static inline int screen_to_board_index(int idx, bool black_perspective)
{
    return black_perspective ? (CHESS_BOARD_SIZE - 1 - idx) : idx;
}

static inline bool board_square_is_light(int file, int rank)
{
    return ((file + rank) % 2) == 0;
}

/* ------------------------------------------------------------------ */
/*  Shared module-level state (defined in one TU, extern elsewhere)    */
/* ------------------------------------------------------------------ */

/* Cached rect for the "Return to Lobby" button inside the game-over overlay.
 * Written by render_game_over_banner() in game.c,
 * read by chess_ui_game_button_from_mouse() in game_panels.c. */
extern SDL_FRect s_lobby_button_rect;
extern bool      s_lobby_button_visible;

/* Cursor X where captured pieces start drawing, per panel.
 * Written by render_one_player_panel() in game_panels.c,
 * read by render_capture_animation() in game_animations.c. */
extern float s_cap_cursor_start_top;
extern float s_cap_cursor_start_bottom;

/* ------------------------------------------------------------------ */
/*  Internal rendering functions called by chess_ui_render_frame()      */
/* ------------------------------------------------------------------ */

bool chess_ui_promotion_choice_rect(
    AppContext *ctx, int width, int board_y, int board_height,
    uint8_t promotion, SDL_FRect *out_rect);

void chess_ui_render_board_coordinates(
    SDL_Renderer *renderer, int width, int board_y, int board_height,
    ChessPlayerColor local_color);

void chess_ui_render_game_overlay(
    SDL_Renderer *renderer, int width, int board_y, int board_height,
    const ChessGameState *game_state, ChessPlayerColor local_color,
    bool hide_piece, int hidden_file, int hidden_rank);

void chess_ui_render_drag_preview(AppContext *ctx, int width, int board_height);

void chess_ui_render_promotion_overlay(
    AppContext *ctx, int width, int board_y, int board_height);

void chess_ui_render_game_over_banner(
    AppContext *ctx, int width, int board_y, int board_height);

void chess_ui_render_status_message(AppContext *ctx, int width, int board_y);

void chess_ui_render_player_panels(
    AppContext *ctx, int board_width, int window_height,
    int board_y, int board_height);

void chess_ui_render_move_history_panel(
    AppContext *ctx, int window_width, int window_height, int board_width);

void chess_ui_render_remote_move_animation(
    AppContext *ctx, int width, int board_y, int board_height);

void chess_ui_render_capture_animation(
    AppContext *ctx, int board_width, int board_y, int board_height);

void chess_ui_render_snap_back_animation(
    AppContext *ctx, int width, int board_y, int board_height);

#endif /* CHESS_APP_UI_GAME_INTERNAL_H */
