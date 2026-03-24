#include "chess_app/ui_fonts.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>
#include <string.h>

/* ── Global resource pool (declared extern in ui_fonts.h) ─────────── */

TTF_Font    *s_chess_font                     = NULL;
TTF_Font    *s_coord_font                     = NULL;
TTF_Font    *s_lobby_font                     = NULL;
SDL_Texture *s_piece_textures[CHESS_PIECE_COUNT];
SDL_Texture *s_file_label_textures[CHESS_BOARD_SIZE][2];
SDL_Texture *s_rank_label_textures[CHESS_BOARD_SIZE][2];
bool         s_ttf_initialized                = false;
bool         s_lobby_icon_pending_available   = false;
bool         s_lobby_icon_incoming_available  = false;
bool         s_lobby_icon_matched_available   = false;
const char  *s_lobby_font_path                = NULL;

/* ── Internal helpers ─────────────────────────────────────────────── */

static TTF_Font *open_font_from_candidates(const char * const *font_paths, float font_size)
{
    int i;

    for (i = 0; font_paths[i] != NULL; ++i) {
        TTF_Font *font = TTF_OpenFont(font_paths[i], font_size);
        if (font) {
            SDL_Log("UI: loaded font %s (size=%.1f)", font_paths[i], (double)font_size);
            return font;
        }
    }
    return NULL;
}

static bool text_surface_equals(SDL_Surface *a, SDL_Surface *b)
{
    int y;

    if (!a || !b) {
        return false;
    }

    if (a->w != b->w || a->h != b->h || a->pitch != b->pitch || a->format != b->format) {
        return false;
    }

    for (y = 0; y < a->h; ++y) {
        const uint8_t *row_a = (const uint8_t *)a->pixels + y * a->pitch;
        const uint8_t *row_b = (const uint8_t *)b->pixels + y * b->pitch;
        if (memcmp(row_a, row_b, (size_t)a->pitch) != 0) {
            return false;
        }
    }

    return true;
}

static bool font_supports_icon(TTF_Font *font, const char *icon_utf8)
{
    const SDL_Color color = {255, 255, 255, 255};
    SDL_Surface *icon = NULL;
    SDL_Surface *tofu_square = NULL;
    SDL_Surface *question = NULL;
    bool supported = false;

    if (!font) {
        return false;
    }

    icon = TTF_RenderText_Blended(font, icon_utf8, SDL_strlen(icon_utf8), color);
    tofu_square = TTF_RenderText_Blended(font, "\xe2\x96\xa1", SDL_strlen("\xe2\x96\xa1"), color);
    question = TTF_RenderText_Blended(font, "?", SDL_strlen("?"), color);

    if (!icon || !tofu_square || !question) {
        goto cleanup;
    }

    if (text_surface_equals(icon, tofu_square) || text_surface_equals(icon, question)) {
        goto cleanup;
    }

    supported = true;

cleanup:
    if (icon) {
        SDL_DestroySurface(icon);
    }
    if (tofu_square) {
        SDL_DestroySurface(tofu_square);
    }
    if (question) {
        SDL_DestroySurface(question);
    }

    return supported;
}

static int lobby_icon_coverage_score(TTF_Font *font)
{
    int score = 0;

    if (!font) {
        return 0;
    }

    if (font_supports_icon(font, "\xe2\x8f\xb3")) {
        score += 1;
    }
    if (font_supports_icon(font, "\xe2\x9a\x94")) {
        score += 1;
    }
    if (font_supports_icon(font, "\xe2\x9c\x93")) {
        score += 1;
    }

    return score;
}

static TTF_Font *open_lobby_font_from_candidates(const char * const *font_paths, float font_size, const char **out_font_path)
{
    int i;
    int best_score = 0;
    TTF_Font *best_font = NULL;
    const char *best_path = NULL;

    if (out_font_path) {
        *out_font_path = NULL;
    }

    for (i = 0; font_paths[i] != NULL; ++i) {
        TTF_Font *font = TTF_OpenFont(font_paths[i], font_size);
        int score;
        if (!font) {
            continue;
        }

        score = lobby_icon_coverage_score(font);
        if (score > best_score) {
            if (best_font) {
                TTF_CloseFont(best_font);
            }
            best_font = font;
            best_path = font_paths[i];
            best_score = score;
            continue;
        }

        TTF_CloseFont(font);
    }

    if (best_font) {
        SDL_Log(
            "UI: loaded best lobby icon font %s (size=%.1f, coverage=%d/3)",
            best_path ? best_path : "(unknown)",
            (double)font_size,
            best_score
        );
        if (out_font_path) {
            *out_font_path = best_path;
        }
        return best_font;
    }

    return NULL;
}

static SDL_Texture *make_outlined_glyph_texture(
    SDL_Renderer *renderer,
    TTF_Font     *font,
    Uint32        codepoint,
    SDL_Color     fg,
    SDL_Color     outline_col,
    int           outline_px)
{
    SDL_Surface *outline_surf = NULL;
    SDL_Surface *fill_surf    = NULL;
    SDL_Surface *combined     = NULL;
    SDL_Texture *tex          = NULL;
    SDL_Rect     fill_dst;

    TTF_SetFontOutline(font, outline_px);
    outline_surf = TTF_RenderGlyph_Blended(font, codepoint, outline_col);
    TTF_SetFontOutline(font, 0);
    fill_surf    = TTF_RenderGlyph_Blended(font, codepoint, fg);

    if (!outline_surf || !fill_surf) {
        goto cleanup;
    }

    combined = SDL_CreateSurface(outline_surf->w, outline_surf->h, SDL_PIXELFORMAT_ARGB8888);
    if (!combined) {
        goto cleanup;
    }
    SDL_FillSurfaceRect(combined, NULL, 0u);

    SDL_SetSurfaceBlendMode(outline_surf, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(outline_surf, NULL, combined, NULL);

    fill_dst.x = outline_px;
    fill_dst.y = outline_px;
    fill_dst.w = fill_surf->w;
    fill_dst.h = fill_surf->h;
    SDL_SetSurfaceBlendMode(fill_surf, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(fill_surf, NULL, combined, &fill_dst);

    tex = SDL_CreateTextureFromSurface(renderer, combined);
    SDL_DestroySurface(combined);
    combined = NULL;

cleanup:
    if (outline_surf) SDL_DestroySurface(outline_surf);
    if (fill_surf)    SDL_DestroySurface(fill_surf);
    if (combined)     SDL_DestroySurface(combined);
    return tex;
}

/* ── Public API ───────────────────────────────────────────────────── */

SDL_Texture *make_text_texture(SDL_Renderer *renderer, TTF_Font *font, const char *text, SDL_Color color)
{
    SDL_Surface *surface;
    SDL_Texture *texture;

    if (!renderer || !font || !text) {
        return NULL;
    }

    surface = TTF_RenderText_Blended(font, text, SDL_strlen(text), color);
    if (!surface) {
        return NULL;
    }

    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

const char *lobby_state_suffix(ChessChallengeState state)
{
    switch (state) {
    case CHESS_CHALLENGE_NONE:
        return "";
    case CHESS_CHALLENGE_OUTGOING_PENDING:
        return s_lobby_icon_pending_available ? " [\xe2\x8f\xb3]" : " [PENDING]";
    case CHESS_CHALLENGE_INCOMING_PENDING:
        return s_lobby_icon_incoming_available ? " [\xe2\x9a\x94]" : " [INCOMING]";
    case CHESS_CHALLENGE_MATCHED:
        return s_lobby_icon_matched_available ? " [\xe2\x9c\x93]" : " [MATCHED]";
    }

    return "";
}

void init_piece_textures(SDL_Renderer *renderer)
{
    static const char * const chess_font_paths[] = {
        /* macOS */
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
        /* Linux */
        "/usr/share/fonts/truetype/ancient-scripts/Symbola_hint.ttf",
        "/usr/local/share/fonts/FreeSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        NULL
    };
    static const char * const coord_font_paths[] = {
        /* macOS */
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
        /* Linux */
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/local/share/fonts/FreeSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        NULL
    };
    static const char * const lobby_icon_font_paths[] = {
        /* macOS */
        "/System/Library/Fonts/Supplemental/STIXTwoMath.otf",
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/CJKSymbolsFallback.ttc",
        "/System/Library/Fonts/Symbol.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/Library/Fonts/NotoSansSymbols2-Regular.ttf",
        "/System/Library/Fonts/Supplemental/NotoSansSymbols2-Regular.ttf",
        /* Linux */
        "/usr/share/fonts/truetype/ancient-scripts/Symbola_hint.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/local/share/fonts/FreeSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/urw-base35/StandardSymbolsPS.otf",
        NULL
    };
    static const Uint32 piece_codepoints[CHESS_PIECE_COUNT] = {
        [CHESS_PIECE_WHITE_PAWN]   = 0x2659u,
        [CHESS_PIECE_WHITE_KNIGHT] = 0x2658u,
        [CHESS_PIECE_WHITE_BISHOP] = 0x2657u,
        [CHESS_PIECE_WHITE_ROOK]   = 0x2656u,
        [CHESS_PIECE_WHITE_QUEEN]  = 0x2655u,
        [CHESS_PIECE_WHITE_KING]   = 0x2654u,
        [CHESS_PIECE_BLACK_PAWN]   = 0x265Fu,
        [CHESS_PIECE_BLACK_KNIGHT] = 0x265Eu,
        [CHESS_PIECE_BLACK_BISHOP] = 0x265Du,
        [CHESS_PIECE_BLACK_ROOK]   = 0x265Cu,
        [CHESS_PIECE_BLACK_QUEEN]  = 0x265Bu,
        [CHESS_PIECE_BLACK_KING]   = 0x265Au,
    };
    const SDL_Color white_fg      = {245, 238, 200, 255};
    const SDL_Color white_outline = { 50,  35,  20, 255};
    const SDL_Color black_fg      = { 45,  30,  20, 255};
    const SDL_Color black_outline = {215, 210, 175, 255};
    const SDL_Color coord_on_light = {60, 70, 45, 255};
    const SDL_Color coord_on_dark  = {238, 238, 210, 255};
    int i;
    float font_size = 52.0f;

    if (!TTF_Init()) {
        SDL_Log("UI: TTF_Init failed: %s", SDL_GetError());
        return;
    }
    s_ttf_initialized = true;

    s_chess_font = open_font_from_candidates(chess_font_paths, font_size);
    if (!s_chess_font) {
        SDL_Log("UI: no chess font found, piece rendering will use fallback rectangles");
    }

    if (s_chess_font) {
        for (i = 1; i < CHESS_PIECE_COUNT; ++i) {
            bool is_white = (i < (int)CHESS_PIECE_BLACK_PAWN);
            SDL_Color fg      = is_white ? white_fg      : black_fg;
            SDL_Color outline = is_white ? white_outline : black_outline;
            s_piece_textures[i] = make_outlined_glyph_texture(
                renderer, s_chess_font, piece_codepoints[i], fg, outline, 2);
            if (!s_piece_textures[i]) {
                SDL_Log("UI: failed to create texture for piece %d: %s", i, SDL_GetError());
            }
        }
    }

    s_coord_font = open_font_from_candidates(coord_font_paths, 16.0f);
    if (!s_coord_font) {
        SDL_Log("UI: no coordinate font found, board coordinates disabled");
    } else {
        for (i = 0; i < CHESS_BOARD_SIZE; ++i) {
            char file_label[2] = { (char)('a' + i), '\0' };
            char rank_label[2] = { (char)('8' - i), '\0' };

            s_file_label_textures[i][0] = make_text_texture(renderer, s_coord_font, file_label, coord_on_light);
            s_file_label_textures[i][1] = make_text_texture(renderer, s_coord_font, file_label, coord_on_dark);
            s_rank_label_textures[i][0] = make_text_texture(renderer, s_coord_font, rank_label, coord_on_light);
            s_rank_label_textures[i][1] = make_text_texture(renderer, s_coord_font, rank_label, coord_on_dark);
        }
    }

    s_lobby_font = open_lobby_font_from_candidates(lobby_icon_font_paths, 16.0f, &s_lobby_font_path);
    if (s_lobby_font) {
        s_lobby_icon_pending_available = font_supports_icon(s_lobby_font, "\xe2\x8f\xb3");
        s_lobby_icon_incoming_available = font_supports_icon(s_lobby_font, "\xe2\x9a\x94");
        s_lobby_icon_matched_available = font_supports_icon(s_lobby_font, "\xe2\x9c\x93");
        SDL_Log(
            "UI: lobby rendering font %s (icons: pending=%s incoming=%s matched=%s)",
            s_lobby_font_path ? s_lobby_font_path : "(unknown)",
            s_lobby_icon_pending_available ? "yes" : "no",
            s_lobby_icon_incoming_available ? "yes" : "no",
            s_lobby_icon_matched_available ? "yes" : "no"
        );
    } else {
        s_lobby_icon_pending_available = false;
        s_lobby_icon_incoming_available = false;
        s_lobby_icon_matched_available = false;
        s_lobby_font = s_coord_font;
        s_lobby_font_path = "(fallback: coordinate font)";
        SDL_Log("UI: no icon-capable lobby font found, using ASCII fallback labels");
        SDL_Log("UI: lobby rendering font %s", s_lobby_font_path);
    }
}

void destroy_piece_textures(void)
{
    int i;
    for (i = 1; i < CHESS_PIECE_COUNT; ++i) {
        if (s_piece_textures[i]) {
            SDL_DestroyTexture(s_piece_textures[i]);
            s_piece_textures[i] = NULL;
        }
    }

    for (i = 0; i < CHESS_BOARD_SIZE; ++i) {
        if (s_file_label_textures[i][0]) {
            SDL_DestroyTexture(s_file_label_textures[i][0]);
            s_file_label_textures[i][0] = NULL;
        }
        if (s_file_label_textures[i][1]) {
            SDL_DestroyTexture(s_file_label_textures[i][1]);
            s_file_label_textures[i][1] = NULL;
        }
        if (s_rank_label_textures[i][0]) {
            SDL_DestroyTexture(s_rank_label_textures[i][0]);
            s_rank_label_textures[i][0] = NULL;
        }
        if (s_rank_label_textures[i][1]) {
            SDL_DestroyTexture(s_rank_label_textures[i][1]);
            s_rank_label_textures[i][1] = NULL;
        }
    }

    if (s_chess_font) {
        TTF_CloseFont(s_chess_font);
        s_chess_font = NULL;
    }
    if (s_lobby_font && s_lobby_font != s_coord_font) {
        TTF_CloseFont(s_lobby_font);
        s_lobby_font = NULL;
    }
    s_lobby_icon_pending_available = false;
    s_lobby_icon_incoming_available = false;
    s_lobby_icon_matched_available = false;
    s_lobby_font_path = NULL;
    if (s_coord_font) {
        TTF_CloseFont(s_coord_font);
        s_coord_font = NULL;
    }
    if (s_ttf_initialized) {
        TTF_Quit();
        s_ttf_initialized = false;
    }
}
