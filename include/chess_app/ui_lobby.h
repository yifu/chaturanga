#ifndef CHESS_APP_UI_LOBBY_H
#define CHESS_APP_UI_LOBBY_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "chess_app/lobby_state.h"

/**
 * Render the lobby screen (title, peer list, waiting message).
 */
void chess_lobby_render(
    SDL_Renderer *renderer,
    int width,
    int height,
    const ChessLobbyState *lobby,
    TTF_Font *font);

/**
 * Return the index of the lobby peer at the given mouse position, or -1.
 */
int chess_lobby_find_clicked_peer(
    SDL_Window *window,
    const ChessLobbyState *lobby,
    int mouse_x,
    int mouse_y);

#endif /* CHESS_APP_UI_LOBBY_H */
