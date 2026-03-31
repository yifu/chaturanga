#ifndef CHESS_APP_APP_WINDOW_H
#define CHESS_APP_APP_WINDOW_H

#include <SDL3/SDL.h>

typedef struct AppWindow {
    int window_size;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Cursor *cursor_default;
    SDL_Cursor *cursor_pointer;
} AppWindow;

#endif
