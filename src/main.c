#include <SDL3/SDL.h>
#include <stdbool.h>

int main(void)
{
    const int window_size = 640;
    const int board_size = 8;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDL3 Chess Board", window_size, window_size, 0);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);

        const float cell_w = (float)width / (float)board_size;
        const float cell_h = (float)height / (float)board_size;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        for (int y = 0; y < board_size; ++y) {
            for (int x = 0; x < board_size; ++x) {
                const bool is_white = ((x + y) % 2) == 0;
                if (is_white) {
                    SDL_SetRenderDrawColor(renderer, 238, 238, 210, 255);
                } else {
                    SDL_SetRenderDrawColor(renderer, 118, 150, 86, 255);
                }

                SDL_FRect rect = {
                    x * cell_w,
                    y * cell_h,
                    cell_w,
                    cell_h
                };
                SDL_RenderFillRect(renderer, &rect);
            }
        }

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
