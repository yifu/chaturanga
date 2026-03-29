#include "chess_app/ui_fonts.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "embedded_pieces.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>
#include <stdbool.h>
#include <string.h>

/* ── Global resource pool (declared extern in ui_fonts.h) ─────────── */

TTF_Font    *s_coord_font                     = NULL;
TTF_Font    *s_lobby_font                     = NULL;
SDL_Texture *s_piece_textures[CHESS_PIECE_COUNT];
SDL_Texture *s_piece_silhouettes[CHESS_PIECE_COUNT];
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
    case CHESS_CHALLENGE_CONNECT_FAILED:
        return " [\xe2\x9a\xa0 UNREACHABLE]";
    }

    return "";
}

static SDL_Texture *load_embedded_png(SDL_Renderer *renderer,
                                       const unsigned char *data,
                                       size_t size)
{
    SDL_IOStream *io = SDL_IOFromConstMem(data, size);
    if (!io) {
        SDL_Log("UI: SDL_IOFromConstMem failed: %s", SDL_GetError());
        return NULL;
    }
    SDL_Texture *tex = IMG_LoadTexture_IO(renderer, io, true);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    return tex;
}

/* Create a white silhouette texture: same alpha channel, all RGB set to white.
 * Used to draw a visible halo behind dark pieces on dark backgrounds. */
static SDL_Texture *make_white_silhouette(SDL_Renderer *renderer,
                                          const unsigned char *data,
                                          size_t size)
{
    SDL_IOStream *io = SDL_IOFromConstMem(data, size);
    SDL_Surface *surf;
    SDL_Surface *converted;
    SDL_Texture *tex;
    int x, y;

    if (!io) {
        return NULL;
    }
    surf = IMG_Load_IO(io, true);
    if (!surf) {
        return NULL;
    }
    converted = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(surf);
    if (!converted) {
        return NULL;
    }
    /* Set all RGB to white, keep alpha untouched */
    for (y = 0; y < converted->h; ++y) {
        uint32_t *row = (uint32_t *)((uint8_t *)converted->pixels + y * converted->pitch);
        for (x = 0; x < converted->w; ++x) {
            uint32_t a = row[x] & 0xFF000000u;
            row[x] = a | 0x00FFFFFFu;
        }
    }
    tex = SDL_CreateTextureFromSurface(renderer, converted);
    SDL_DestroySurface(converted);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    return tex;
}

void init_piece_textures(SDL_Renderer *renderer)
{
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
    static const struct {
        int piece;
        const unsigned char *data;
        const size_t *size;
    } embedded_pieces[] = {
        { CHESS_PIECE_WHITE_PAWN,   embedded_Chess_plt60, &embedded_Chess_plt60_size },
        { CHESS_PIECE_WHITE_KNIGHT, embedded_Chess_nlt60, &embedded_Chess_nlt60_size },
        { CHESS_PIECE_WHITE_BISHOP, embedded_Chess_blt60, &embedded_Chess_blt60_size },
        { CHESS_PIECE_WHITE_ROOK,   embedded_Chess_rlt60, &embedded_Chess_rlt60_size },
        { CHESS_PIECE_WHITE_QUEEN,  embedded_Chess_qlt60, &embedded_Chess_qlt60_size },
        { CHESS_PIECE_WHITE_KING,   embedded_Chess_klt60, &embedded_Chess_klt60_size },
        { CHESS_PIECE_BLACK_PAWN,   embedded_Chess_pdt60, &embedded_Chess_pdt60_size },
        { CHESS_PIECE_BLACK_KNIGHT, embedded_Chess_ndt60, &embedded_Chess_ndt60_size },
        { CHESS_PIECE_BLACK_BISHOP, embedded_Chess_bdt60, &embedded_Chess_bdt60_size },
        { CHESS_PIECE_BLACK_ROOK,   embedded_Chess_rdt60, &embedded_Chess_rdt60_size },
        { CHESS_PIECE_BLACK_QUEEN,  embedded_Chess_qdt60, &embedded_Chess_qdt60_size },
        { CHESS_PIECE_BLACK_KING,   embedded_Chess_kdt60, &embedded_Chess_kdt60_size },
    };
    const SDL_Color coord_on_light = {60, 70, 45, 255};
    const SDL_Color coord_on_dark  = {238, 238, 210, 255};
    int i;

    if (!TTF_Init()) {
        SDL_Log("UI: TTF_Init failed: %s", SDL_GetError());
        return;
    }
    s_ttf_initialized = true;

    /* Load embedded PNG piece sprites */
    for (i = 0; i < (int)(sizeof(embedded_pieces) / sizeof(embedded_pieces[0])); ++i) {
        int idx = embedded_pieces[i].piece;
        s_piece_textures[idx] =
            load_embedded_png(renderer, embedded_pieces[i].data, *embedded_pieces[i].size);
        if (!s_piece_textures[idx]) {
            SDL_Log("UI: failed to load embedded PNG for piece %d: %s",
                    idx, SDL_GetError());
        }
        /* White silhouette for dark pieces (used as halo on dark backgrounds) */
        if (idx >= (int)CHESS_PIECE_BLACK_PAWN && idx <= (int)CHESS_PIECE_BLACK_KING) {
            s_piece_silhouettes[idx] =
                make_white_silhouette(renderer, embedded_pieces[i].data, *embedded_pieces[i].size);
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
        if (s_piece_silhouettes[i]) {
            SDL_DestroyTexture(s_piece_silhouettes[i]);
            s_piece_silhouettes[i] = NULL;
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
