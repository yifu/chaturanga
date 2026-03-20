#include "chess_app/render_board.h"

#include <stdbool.h>

void render_board(SDL_Renderer *renderer, int width, int height)
{
    const int board_size = 8;
    const float cell_w = (float)width / (float)board_size;
    const float cell_h = (float)height / (float)board_size;

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
}
