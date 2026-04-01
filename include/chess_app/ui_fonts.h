#ifndef CHESS_APP_UI_FONTS_H
#define CHESS_APP_UI_FONTS_H

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>

/* ── Global font / texture resources (owned by ui/fonts.c) ────────── */

extern TTF_Font    *s_coord_font;
extern TTF_Font    *s_lobby_font;
extern SDL_Texture *s_piece_textures[CHESS_PIECE_COUNT];
extern SDL_Texture *s_piece_silhouettes[CHESS_PIECE_COUNT];
extern SDL_Texture *s_file_label_textures[CHESS_BOARD_SIZE][2];
extern SDL_Texture *s_rank_label_textures[CHESS_BOARD_SIZE][2];
extern bool         s_ttf_initialized;
extern bool         s_lobby_icon_pending_available;
extern bool         s_lobby_icon_incoming_available;
extern bool         s_lobby_icon_matched_available;
extern const char  *s_lobby_font_path;
extern SDL_Texture *s_nerve_texture;

/* ── Public API ───────────────────────────────────────────────────── */

void         init_piece_textures(SDL_Renderer *renderer);
void         destroy_piece_textures(void);
SDL_Texture *make_text_texture(SDL_Renderer *renderer, TTF_Font *font, const char *text, SDL_Color color);
const char  *lobby_state_suffix(ChessChallengeState state);

#endif
