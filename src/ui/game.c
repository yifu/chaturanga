#include "chess_app/ui_game.h"
#include "chess_app/ui_fonts.h"
#include "chess_app/ui_lobby.h"
#include "chess_app/render_board.h"
#include "chess_app/network_session.h"

#include <SDL3_ttf/SDL_ttf.h>

/* ------------------------------------------------------------------ */
/*  Internal constants                                                 */
/* ------------------------------------------------------------------ */

static const int s_history_max_entries = (int)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES;
static const int s_history_entry_len   = (int)CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN;

/* Cached rect for the "Return to Lobby" button inside the game-over overlay. */
static SDL_FRect s_lobby_button_rect = {0};
static bool      s_lobby_button_visible = false;

/* Forward declarations */
static void render_panel_buttons(AppContext *ctx, int panel_left, int panel_width, int window_height);
static void render_player_panels(AppContext *ctx, int board_width, int window_height, int board_y, int board_height);

/* ------------------------------------------------------------------ */
/*  Pure coordinate / perspective helpers (static)                     */
/* ------------------------------------------------------------------ */

static bool use_black_perspective(ChessPlayerColor local_color)
{
    return local_color == CHESS_COLOR_BLACK;
}

static int board_to_screen_index(int idx, bool black_perspective)
{
    return black_perspective ? (CHESS_BOARD_SIZE - 1 - idx) : idx;
}

static int screen_to_board_index(int idx, bool black_perspective)
{
    return black_perspective ? (CHESS_BOARD_SIZE - 1 - idx) : idx;
}

static bool board_square_is_light(int file, int rank)
{
    return ((file + rank) % 2) == 0;
}

static ChessPiece promotion_choice_piece(ChessPlayerColor color, uint8_t promotion)
{
    if (color == CHESS_COLOR_WHITE) {
        switch (promotion) {
        case CHESS_PROMOTION_QUEEN:
            return CHESS_PIECE_WHITE_QUEEN;
        case CHESS_PROMOTION_ROOK:
            return CHESS_PIECE_WHITE_ROOK;
        case CHESS_PROMOTION_BISHOP:
            return CHESS_PIECE_WHITE_BISHOP;
        case CHESS_PROMOTION_KNIGHT:
            return CHESS_PIECE_WHITE_KNIGHT;
        default:
            return CHESS_PIECE_EMPTY;
        }
    }

    if (color == CHESS_COLOR_BLACK) {
        switch (promotion) {
        case CHESS_PROMOTION_QUEEN:
            return CHESS_PIECE_BLACK_QUEEN;
        case CHESS_PROMOTION_ROOK:
            return CHESS_PIECE_BLACK_ROOK;
        case CHESS_PROMOTION_BISHOP:
            return CHESS_PIECE_BLACK_BISHOP;
        case CHESS_PROMOTION_KNIGHT:
            return CHESS_PIECE_BLACK_KNIGHT;
        default:
            return CHESS_PIECE_EMPTY;
        }
    }

    return CHESS_PIECE_EMPTY;
}

/* ------------------------------------------------------------------ */
/*  Public: board width                                                */
/* ------------------------------------------------------------------ */

int chess_ui_board_width_for_window(int window_width, bool game_started)
{
    int board_width;

    if (!game_started) {
        return window_width;
    }

    board_width = window_width - CHESS_UI_HISTORY_PANEL_WIDTH;
    if (board_width <= 0) {
        return window_width;
    }
    return board_width;
}

/* ------------------------------------------------------------------ */
/*  Public: screen to board conversion                                 */
/* ------------------------------------------------------------------ */

bool chess_ui_screen_to_board_square(
    AppContext *ctx, int mouse_x, int mouse_y, int *out_file, int *out_rank)
{
    int width = 0;
    int board_width = 0;
    int height = 0;
    int board_y = 0;
    int board_height;
    int local_mouse_y;
    float cell_w;
    float cell_h;
    bool black_perspective;
    int screen_file;
    int screen_rank;

    if (!ctx || !ctx->window || !out_file || !out_rank) {
        return false;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network_session.game_started);
    if (mouse_x < 0 || mouse_x >= board_width) {
        return false;
    }

    if (ctx->network_session.game_started) {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
    } else {
        board_height = height;
    }
    if (board_height <= 0) {
        return false;
    }

    local_mouse_y = mouse_y - board_y;
    if (local_mouse_y < 0 || local_mouse_y >= board_height) {
        return false;
    }

    cell_w = (float)board_width / (float)CHESS_BOARD_SIZE;
    cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    black_perspective = use_black_perspective(ctx->network_session.local_color);
    screen_file = (int)(mouse_x / cell_w);
    screen_rank = (int)(local_mouse_y / cell_h);
    *out_file = screen_to_board_index(screen_file, black_perspective);
    *out_rank = screen_to_board_index(screen_rank, black_perspective);

    if (*out_file < 0 || *out_file >= CHESS_BOARD_SIZE || *out_rank < 0 || *out_rank >= CHESS_BOARD_SIZE) {
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Promotion helpers                                                  */
/* ------------------------------------------------------------------ */

static bool promotion_choice_rect(
    AppContext *ctx,
    int width,
    int board_y,
    int board_height,
    uint8_t promotion,
    SDL_FRect *out_rect)
{
    bool black_perspective;
    float cell_w;
    float cell_h;
    int screen_file;
    int screen_rank;
    SDL_FRect square_rect;

    if (!ctx || !out_rect || !ctx->promotion_pending) {
        return false;
    }

    black_perspective = use_black_perspective(ctx->network_session.local_color);
    cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    screen_file = board_to_screen_index(ctx->promotion_to_file, black_perspective);
    screen_rank = board_to_screen_index(ctx->promotion_to_rank, black_perspective);

    square_rect.x = screen_file * cell_w + 3.0f;
    square_rect.y = (float)board_y + screen_rank * cell_h + 3.0f;
    square_rect.w = cell_w - 6.0f;
    square_rect.h = cell_h - 6.0f;

    out_rect->w = square_rect.w * 0.5f;
    out_rect->h = square_rect.h * 0.5f;

    switch (promotion) {
    case CHESS_PROMOTION_QUEEN:
        out_rect->x = square_rect.x;
        out_rect->y = square_rect.y;
        return true;
    case CHESS_PROMOTION_ROOK:
        out_rect->x = square_rect.x + out_rect->w;
        out_rect->y = square_rect.y;
        return true;
    case CHESS_PROMOTION_BISHOP:
        out_rect->x = square_rect.x;
        out_rect->y = square_rect.y + out_rect->h;
        return true;
    case CHESS_PROMOTION_KNIGHT:
        out_rect->x = square_rect.x + out_rect->w;
        out_rect->y = square_rect.y + out_rect->h;
        return true;
    default:
        return false;
    }
}

uint8_t chess_ui_promotion_from_mouse(AppContext *ctx, int mouse_x, int mouse_y)
{
    const uint8_t choices[] = {
        CHESS_PROMOTION_QUEEN,
        CHESS_PROMOTION_ROOK,
        CHESS_PROMOTION_BISHOP,
        CHESS_PROMOTION_KNIGHT
    };
    int width;
    int board_width;
    int height;
    int board_y = 0;
    int board_height;
    size_t i;

    if (!ctx || !ctx->window || !ctx->promotion_pending) {
        return CHESS_PROMOTION_NONE;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network_session.game_started);
    if (ctx->network_session.game_started) {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
    } else {
        board_height = height;
    }
    for (i = 0; i < SDL_arraysize(choices); ++i) {
        SDL_FRect rect;
        if (promotion_choice_rect(ctx, board_width, board_y, board_height, choices[i], &rect) &&
            mouse_x >= (int)rect.x &&
            mouse_x < (int)(rect.x + rect.w) &&
            mouse_y >= (int)rect.y &&
            mouse_y < (int)(rect.y + rect.h)) {
            return choices[i];
        }
    }

    return CHESS_PROMOTION_NONE;
}

/* ------------------------------------------------------------------ */
/*  Board coordinate labels                                            */
/* ------------------------------------------------------------------ */

static void render_board_coordinates(
    SDL_Renderer *renderer,
    int width,
    int board_y,
    int board_height,
    ChessPlayerColor local_color)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(local_color);
    int screen_file;
    int screen_rank;

    if (!renderer || !s_coord_font) {
        return;
    }

    for (screen_file = 0; screen_file < CHESS_BOARD_SIZE; ++screen_file) {
        int board_file = screen_to_board_index(screen_file, black_perspective);
        int board_rank = screen_to_board_index(CHESS_BOARD_SIZE - 1, black_perspective);
        int color_idx = board_square_is_light(board_file, board_rank) ? 0 : 1;
        SDL_Texture *label_tex = s_file_label_textures[board_file][color_idx];
        if (label_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(label_tex, &tex_w, &tex_h);
            dst.x = screen_file * cell_w + cell_w - tex_w - 4.0f;
            dst.y = (float)board_y + (CHESS_BOARD_SIZE - 1) * cell_h + cell_h - tex_h - 2.0f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, label_tex, NULL, &dst);
        }
    }

    for (screen_rank = 0; screen_rank < CHESS_BOARD_SIZE; ++screen_rank) {
        int board_rank = screen_to_board_index(screen_rank, black_perspective);
        int board_file = screen_to_board_index(0, black_perspective);
        int color_idx = board_square_is_light(board_file, board_rank) ? 0 : 1;
        SDL_Texture *label_tex = s_rank_label_textures[board_rank][color_idx];
        if (label_tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(label_tex, &tex_w, &tex_h);
            dst.x = 4.0f;
            dst.y = (float)board_y + screen_rank * cell_h + 2.0f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(renderer, label_tex, NULL, &dst);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Game overlay (piece sprites on board)                              */
/* ------------------------------------------------------------------ */

static void render_game_overlay(
    SDL_Renderer *renderer,
    int width,
    int board_y,
    int board_height,
    const ChessGameState *game_state,
    ChessPlayerColor local_color,
    bool hide_piece,
    int hidden_file,
    int hidden_rank)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(local_color);
    int rank;
    int file;

    if (!renderer || !game_state) {
        return;
    }

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            ChessPiece piece = chess_game_get_piece(game_state, file, rank);
            if (piece == CHESS_PIECE_EMPTY) {
                continue;
            }

            if (hide_piece && file == hidden_file && rank == hidden_rank) {
                continue;
            }

            {
                SDL_Texture *tex = ((int)piece > 0 && (int)piece < CHESS_PIECE_COUNT)
                    ? s_piece_textures[(int)piece]
                    : NULL;

                if (tex) {
                    float tex_w = 0.0f;
                    float tex_h = 0.0f;
                    int screen_file = board_to_screen_index(file, black_perspective);
                    int screen_rank = board_to_screen_index(rank, black_perspective);
                    SDL_FRect dst;
                    SDL_GetTextureSize(tex, &tex_w, &tex_h);
                    dst.x = screen_file * cell_w + (cell_w - tex_w) * 0.5f;
                    dst.y = (float)board_y + screen_rank * cell_h + (cell_h - tex_h) * 0.5f;
                    dst.w = tex_w;
                    dst.h = tex_h;
                    SDL_RenderTexture(renderer, tex, NULL, &dst);
                } else {
                    /* Fallback: coloured rectangle when font unavailable */
                    int screen_file = board_to_screen_index(file, black_perspective);
                    int screen_rank = board_to_screen_index(rank, black_perspective);
                    SDL_FRect pawn_rect = {
                        screen_file * cell_w + (cell_w * 0.25f),
                        (float)board_y + screen_rank * cell_h + (cell_h * 0.25f),
                        cell_w * 0.5f,
                        cell_h * 0.5f
                    };
                    if ((int)piece < (int)CHESS_PIECE_BLACK_PAWN) {
                        SDL_SetRenderDrawColor(renderer, 245, 245, 245, 255);
                    } else {
                        SDL_SetRenderDrawColor(renderer, 25, 25, 25, 255);
                    }
                    SDL_RenderFillRect(renderer, &pawn_rect);
                }
            }
        }
    }

    if (game_state->has_selection) {
        int screen_file = board_to_screen_index(game_state->selected_file, black_perspective);
        int screen_rank = board_to_screen_index(game_state->selected_rank, black_perspective);
        SDL_FRect selected_rect = {
            screen_file * cell_w + 2.0f,
            (float)board_y + screen_rank * cell_h + 2.0f,
            cell_w - 4.0f,
            cell_h - 4.0f
        };
        SDL_SetRenderDrawColor(renderer, 255, 204, 0, 255);
        SDL_RenderRect(renderer, &selected_rect);
    }
}

/* ------------------------------------------------------------------ */
/*  Drag preview                                                       */
/* ------------------------------------------------------------------ */

static void render_drag_preview(AppContext *ctx, int width, int board_height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;

    if (!ctx || !ctx->renderer || !ctx->drag_active ||
        ctx->drag_piece <= CHESS_PIECE_EMPTY || ctx->drag_piece >= CHESS_PIECE_COUNT) {
        return;
    }

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->drag_piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = (float)ctx->drag_mouse_x - tex_w * 0.5f;
            dst.y = (float)ctx->drag_mouse_y - tex_h * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                (float)ctx->drag_mouse_x - cell_w * 0.25f,
                (float)ctx->drag_mouse_y - cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->drag_piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->renderer, &piece_rect);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Promotion overlay                                                  */
/* ------------------------------------------------------------------ */

static void render_promotion_overlay(AppContext *ctx, int width, int board_y, int board_height)
{
    const uint8_t choices[] = {
        CHESS_PROMOTION_QUEEN,
        CHESS_PROMOTION_ROOK,
        CHESS_PROMOTION_BISHOP,
        CHESS_PROMOTION_KNIGHT
    };
    size_t i;

    if (!ctx || !ctx->renderer || !ctx->promotion_pending) {
        return;
    }

    for (i = 0; i < SDL_arraysize(choices); ++i) {
        SDL_FRect rect;
        ChessPiece piece;

        if (!promotion_choice_rect(ctx, width, board_y, board_height, choices[i], &rect)) {
            continue;
        }

        SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ctx->renderer, 28, 28, 28, 225);
        SDL_RenderFillRect(ctx->renderer, &rect);
        SDL_SetRenderDrawColor(ctx->renderer, 220, 185, 80, 255);
        SDL_RenderRect(ctx->renderer, &rect);

        piece = promotion_choice_piece(ctx->network_session.local_color, choices[i]);
        if (piece != CHESS_PIECE_EMPTY && s_piece_textures[(int)piece]) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(s_piece_textures[(int)piece], &tex_w, &tex_h);
            dst.w = rect.w * 0.78f;
            dst.h = rect.h * 0.78f;
            dst.x = rect.x + (rect.w - dst.w) * 0.5f;
            dst.y = rect.y + (rect.h - dst.h) * 0.5f;
            if (tex_w > 0.0f && tex_h > 0.0f) {
                const float scale = SDL_min(dst.w / tex_w, dst.h / tex_h);
                dst.w = tex_w * scale;
                dst.h = tex_h * scale;
                dst.x = rect.x + (rect.w - dst.w) * 0.5f;
                dst.y = rect.y + (rect.h - dst.h) * 0.5f;
            }
            SDL_RenderTexture(ctx->renderer, s_piece_textures[(int)piece], NULL, &dst);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Remote-move animation                                              */
/* ------------------------------------------------------------------ */

void chess_ui_update_remote_move_animation(AppContext *ctx)
{
    uint64_t now;
    uint64_t elapsed;

    if (!ctx || !ctx->remote_move_anim_active) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->remote_move_anim_started_at_ms;
    if (ctx->remote_move_anim_duration_ms == 0u || elapsed >= (uint64_t)ctx->remote_move_anim_duration_ms) {
        ctx->remote_move_anim_active = false;
        ctx->remote_move_anim_piece = CHESS_PIECE_EMPTY;
        ctx->remote_move_from_file = -1;
        ctx->remote_move_from_rank = -1;
        ctx->remote_move_to_file = -1;
        ctx->remote_move_to_rank = -1;
    }
}

static void render_remote_move_animation(AppContext *ctx, int width, int board_y, int board_height)
{
    const float cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    const float cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    const bool black_perspective = use_black_perspective(ctx->network_session.local_color);
    uint64_t now;
    uint64_t elapsed;
    float t;
    int from_screen_file;
    int from_screen_rank;
    int to_screen_file;
    int to_screen_rank;
    float interp_file;
    float interp_rank;

    if (!ctx || !ctx->renderer || !ctx->remote_move_anim_active ||
        ctx->remote_move_anim_piece <= CHESS_PIECE_EMPTY ||
        ctx->remote_move_anim_piece >= CHESS_PIECE_COUNT) {
        return;
    }

    now = SDL_GetTicks();
    elapsed = now - ctx->remote_move_anim_started_at_ms;
    if (ctx->remote_move_anim_duration_ms == 0u) {
        t = 1.0f;
    } else {
        t = (float)elapsed / (float)ctx->remote_move_anim_duration_ms;
        if (t > 1.0f) {
            t = 1.0f;
        }
    }

    from_screen_file = board_to_screen_index(ctx->remote_move_from_file, black_perspective);
    from_screen_rank = board_to_screen_index(ctx->remote_move_from_rank, black_perspective);
    to_screen_file = board_to_screen_index(ctx->remote_move_to_file, black_perspective);
    to_screen_rank = board_to_screen_index(ctx->remote_move_to_rank, black_perspective);
    interp_file = (1.0f - t) * (float)from_screen_file + t * (float)to_screen_file;
    interp_rank = (1.0f - t) * (float)from_screen_rank + t * (float)to_screen_rank;

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->remote_move_anim_piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = interp_file * cell_w + (cell_w - tex_w) * 0.5f;
            dst.y = (float)board_y + interp_rank * cell_h + (cell_h - tex_h) * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                interp_file * cell_w + cell_w * 0.25f,
                (float)board_y + interp_rank * cell_h + cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->remote_move_anim_piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->renderer, &piece_rect);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Game-over banner                                                   */
/* ------------------------------------------------------------------ */

static void render_game_over_banner(AppContext *ctx, int width, int board_y, int board_height)
{
    const char *headline;
    const char *subline;
    SDL_Texture *headline_tex;
    SDL_Texture *subline_tex;
    float hw;
    float hh;
    float sw;
    float sh;

    if (!ctx || !ctx->renderer || ctx->game_state.outcome == CHESS_OUTCOME_NONE) {
        s_lobby_button_visible = false;
        return;
    }

    headline = NULL;
    subline = NULL;
    switch (ctx->game_state.outcome) {
    case CHESS_OUTCOME_CHECKMATE_WHITE_WINS:
        headline = "Checkmate";
        subline = (ctx->network_session.local_color == CHESS_COLOR_WHITE) ? "You win!" : "Opponent wins";
        break;
    case CHESS_OUTCOME_CHECKMATE_BLACK_WINS:
        headline = "Checkmate";
        subline = (ctx->network_session.local_color == CHESS_COLOR_BLACK) ? "You win!" : "Opponent wins";
        break;
    case CHESS_OUTCOME_STALEMATE:
        headline = "Draw";
        subline = "Stalemate";
        break;
    case CHESS_OUTCOME_FIFTY_MOVE_RULE:
        headline = "Draw";
        subline = "50-move rule";
        break;
    case CHESS_OUTCOME_WHITE_RESIGNED:
        headline = "Resignation";
        subline = (ctx->network_session.local_color == CHESS_COLOR_WHITE) ? "You resigned" : "Opponent resigned";
        break;
    case CHESS_OUTCOME_BLACK_RESIGNED:
        headline = "Resignation";
        subline = (ctx->network_session.local_color == CHESS_COLOR_BLACK) ? "You resigned" : "Opponent resigned";
        break;
    case CHESS_OUTCOME_DRAW_AGREED:
        headline = "Draw";
        subline = "By agreement";
        break;
    default:
        return;
    }

    {
        SDL_FRect overlay;
        overlay.x = 0.0f;
        overlay.y = (float)board_y;
        overlay.w = (float)width;
        overlay.h = (float)board_height;
        SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 160);
        SDL_RenderFillRect(ctx->renderer, &overlay);
        SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_NONE);
    }

    hw = 0.0f;
    hh = 0.0f;
    sw = 0.0f;
    sh = 0.0f;
    headline_tex = make_text_texture(
        ctx->renderer,
        s_lobby_font ? s_lobby_font : s_coord_font,
        headline,
        (SDL_Color){ 255, 255, 255, 255 }
    );
    subline_tex = make_text_texture(
        ctx->renderer,
        s_coord_font ? s_coord_font : s_lobby_font,
        subline,
        (SDL_Color){ 210, 210, 210, 255 }
    );
    if (headline_tex) { SDL_GetTextureSize(headline_tex, &hw, &hh); }
    if (subline_tex)  { SDL_GetTextureSize(subline_tex,  &sw, &sh); }

    {
        const float pad = 28.0f;
        const float gap = 14.0f;
        const float btn_h = 30.0f;
        const float btn_gap = 18.0f;
        const float btn_pad = 24.0f;
        float max_text_w = (hw > sw ? hw : sw);
        float btn_text_w = 0.0f;
        float btn_text_h = 0.0f;
        float box_w;
        float box_h;
        SDL_FRect box;
        SDL_FRect dst;
        SDL_FRect btn_rect;
        SDL_Texture *btn_tex;

        btn_tex = make_text_texture(
            ctx->renderer, s_coord_font, "Return to Lobby",
            (SDL_Color){232, 232, 238, 255});
        if (btn_tex) { SDL_GetTextureSize(btn_tex, &btn_text_w, &btn_text_h); }

        /* Box must be wide enough for all text lines AND the button */
        {
            float btn_needs = btn_text_w + btn_pad * 2.0f;
            if (btn_needs > max_text_w) { max_text_w = btn_needs; }
        }
        box_w = max_text_w + pad * 2.0f;
        box_h = hh + sh + gap + btn_gap + btn_h + pad * 2.0f;

        box.x = ((float)width  - box_w) * 0.5f;
        box.y = (float)board_y + ((float)board_height - box_h) * 0.5f;
        box.w = box_w;
        box.h = box_h;

        SDL_SetRenderDrawColor(ctx->renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(ctx->renderer, &box);
        SDL_SetRenderDrawColor(ctx->renderer, 200, 160, 60, 255);
        SDL_RenderRect(ctx->renderer, &box);

        if (headline_tex) {
            dst.x = box.x + (box.w - hw) * 0.5f;
            dst.y = box.y + pad;
            dst.w = hw;
            dst.h = hh;
            SDL_RenderTexture(ctx->renderer, headline_tex, NULL, &dst);
            SDL_DestroyTexture(headline_tex);
        }
        if (subline_tex) {
            dst.x = box.x + (box.w - sw) * 0.5f;
            dst.y = box.y + pad + hh + gap;
            dst.w = sw;
            dst.h = sh;
            SDL_RenderTexture(ctx->renderer, subline_tex, NULL, &dst);
            SDL_DestroyTexture(subline_tex);
        }

        /* "Return to Lobby" button */
        {
            const float btn_w = box_w - pad * 2.0f;

            btn_rect.x = box.x + pad;
            btn_rect.y = box.y + pad + hh + gap + sh + btn_gap;
            btn_rect.w = btn_w;
            btn_rect.h = btn_h;

            SDL_SetRenderDrawColor(ctx->renderer, 60, 60, 70, 255);
            SDL_RenderFillRect(ctx->renderer, &btn_rect);
            SDL_SetRenderDrawColor(ctx->renderer, 150, 150, 160, 255);
            SDL_RenderRect(ctx->renderer, &btn_rect);

            if (btn_tex) {
                SDL_FRect bdst;
                bdst.x = btn_rect.x + (btn_rect.w - btn_text_w) * 0.5f;
                bdst.y = btn_rect.y + (btn_rect.h - btn_text_h) * 0.5f;
                bdst.w = btn_text_w;
                bdst.h = btn_text_h;
                SDL_RenderTexture(ctx->renderer, btn_tex, NULL, &bdst);
                SDL_DestroyTexture(btn_tex);
            }

            s_lobby_button_rect = btn_rect;
            s_lobby_button_visible = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Move history panel                                                 */
/* ------------------------------------------------------------------ */

static void render_move_history_panel(AppContext *ctx, int window_width, int window_height, int board_width)
{
    SDL_FRect panel_rect;
    SDL_FRect border_rect;
    SDL_FRect header_rect;
    SDL_Texture *title_tex;
    SDL_Texture *turn_tex;
    SDL_Texture *white_header_tex;
    SDL_Texture *black_header_tex;
    float title_w = 0.0f;
    float title_h = 0.0f;
    float turn_w = 0.0f;
    float turn_h = 0.0f;
    float white_header_w = 0.0f;
    float white_header_h = 0.0f;
    float black_header_w = 0.0f;
    float black_header_h = 0.0f;
    int row_height = 20;
    int start_y = 64;
    int max_rows;
    int total_turns;
    int first_turn;
    int turn;
    int panel_left;
    int panel_right;
    int x_turn;
    int x_white;
    int x_black;
    int col_sep_x;
    int current_turn;
    const SDL_Color turn_color = {150, 150, 160, 255};
    const SDL_Color white_black_header_color = {175, 175, 182, 255};
    const SDL_Color normal_move_color = {210, 210, 216, 255};
    const SDL_Color placeholder_color = {112, 112, 120, 255};
    const SDL_Color last_move_color = {236, 201, 104, 255};

    if (!ctx || !ctx->renderer || !s_coord_font || !ctx->network_session.game_started) {
        return;
    }

    panel_rect.x = (float)board_width;
    panel_rect.y = 0.0f;
    panel_rect.w = (float)(window_width - board_width);
    panel_rect.h = (float)window_height;
    if (panel_rect.w <= 1.0f) {
        return;
    }

    panel_left = board_width;
    panel_right = window_width;
    x_turn = panel_left + 10;
    x_white = panel_left + 46;
    x_black = panel_left + (int)(panel_rect.w * 0.56f);
    col_sep_x = panel_left + (int)(panel_rect.w * 0.52f);
    current_turn = ((int)ctx->move_history_count / 2) + 1;

    SDL_SetRenderDrawColor(ctx->renderer, 20, 20, 24, 255);
    SDL_RenderFillRect(ctx->renderer, &panel_rect);

    border_rect.x = (float)board_width;
    border_rect.y = 0.0f;
    border_rect.w = 1.0f;
    border_rect.h = (float)window_height;
    SDL_SetRenderDrawColor(ctx->renderer, 84, 84, 94, 255);
    SDL_RenderFillRect(ctx->renderer, &border_rect);

    header_rect.x = (float)(panel_left + 1);
    header_rect.y = 0.0f;
    header_rect.w = (float)(window_width - panel_left - 1);
    header_rect.h = 56.0f;
    SDL_SetRenderDrawColor(ctx->renderer, 28, 28, 34, 255);
    SDL_RenderFillRect(ctx->renderer, &header_rect);

    title_tex = make_text_texture(ctx->renderer, s_coord_font, "Moves (Ctrl/Cmd+C)", (SDL_Color){232, 232, 238, 255});
    turn_tex = NULL;
    white_header_tex = make_text_texture(ctx->renderer, s_coord_font, "White", white_black_header_color);
    black_header_tex = make_text_texture(ctx->renderer, s_coord_font, "Black", white_black_header_color);

    {
        char turn_label[32];
        SDL_snprintf(turn_label, sizeof(turn_label), "Turn %d", current_turn);
        turn_tex = make_text_texture(ctx->renderer, s_coord_font, turn_label, (SDL_Color){198, 198, 206, 255});
    }

    if (title_tex) {
        SDL_FRect title_dst;
        SDL_GetTextureSize(title_tex, &title_w, &title_h);
        title_dst.x = (float)panel_left + 10.0f;
        title_dst.y = 10.0f;
        title_dst.w = title_w;
        title_dst.h = title_h;
        SDL_RenderTexture(ctx->renderer, title_tex, NULL, &title_dst);
        SDL_DestroyTexture(title_tex);
    }

    if (turn_tex) {
        SDL_FRect turn_dst;
        float min_turn_x = (float)panel_left + 10.0f + title_w + 8.0f;
        SDL_GetTextureSize(turn_tex, &turn_w, &turn_h);
        turn_dst.x = (float)panel_right - turn_w - 10.0f;
        if (turn_dst.x < min_turn_x) {
            turn_dst.x = min_turn_x;
        }
        turn_dst.y = 12.0f;
        turn_dst.w = turn_w;
        turn_dst.h = turn_h;
        SDL_RenderTexture(ctx->renderer, turn_tex, NULL, &turn_dst);
        SDL_DestroyTexture(turn_tex);
    }

    if (white_header_tex) {
        SDL_FRect dst;
        SDL_GetTextureSize(white_header_tex, &white_header_w, &white_header_h);
        dst.x = (float)x_white;
        dst.y = 40.0f;
        dst.w = white_header_w;
        dst.h = white_header_h;
        SDL_RenderTexture(ctx->renderer, white_header_tex, NULL, &dst);
        SDL_DestroyTexture(white_header_tex);
    }

    if (black_header_tex) {
        SDL_FRect dst;
        SDL_GetTextureSize(black_header_tex, &black_header_w, &black_header_h);
        dst.x = (float)x_black;
        dst.y = 40.0f;
        dst.w = black_header_w;
        dst.h = black_header_h;
        SDL_RenderTexture(ctx->renderer, black_header_tex, NULL, &dst);
        SDL_DestroyTexture(black_header_tex);
    }

    SDL_SetRenderDrawColor(ctx->renderer, 74, 74, 84, 255);
    SDL_RenderLine(ctx->renderer, (float)(panel_left + 1), 56.0f, (float)panel_right, 56.0f);
    SDL_RenderLine(ctx->renderer, (float)col_sep_x, 38.0f, (float)col_sep_x, (float)window_height);

    max_rows = (window_height - start_y - 8) / row_height;
    if (max_rows <= 0) {
        render_panel_buttons(ctx, panel_left, (int)panel_rect.w, window_height);
        return;
    }

    total_turns = ((int)ctx->move_history_count + 1) / 2;
    if (total_turns <= 0) {
        SDL_Texture *empty_tex = make_text_texture(
            ctx->renderer,
            s_coord_font,
            "No moves yet",
            (SDL_Color){120, 120, 128, 255});
        if (empty_tex) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(empty_tex, &tw, &th);
            dst.x = (float)panel_left + 12.0f;
            dst.y = 72.0f;
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(ctx->renderer, empty_tex, NULL, &dst);
            SDL_DestroyTexture(empty_tex);
        }
        render_panel_buttons(ctx, panel_left, (int)panel_rect.w, window_height);
        return;
    }

    first_turn = total_turns - max_rows + 1;
    if (first_turn < 1) {
        first_turn = 1;
    }

    for (turn = first_turn; turn <= total_turns; ++turn) {
        int row = turn - first_turn;
        int white_idx = (turn - 1) * 2;
        int black_idx = white_idx + 1;
        bool has_white = white_idx < (int)ctx->move_history_count;
        bool has_black = black_idx < (int)ctx->move_history_count;
        bool last_white = has_white && white_idx == ((int)ctx->move_history_count - 1);
        bool last_black = has_black && black_idx == ((int)ctx->move_history_count - 1);
        const char *white_move = has_white ? ctx->move_history[white_idx] : "";
        const char *black_move = has_black ? ctx->move_history[black_idx] : "...";
        SDL_Texture *turn_tex_row;
        SDL_Texture *white_tex;
        SDL_Texture *black_tex;
        SDL_FRect row_rect;
        char turn_label[12];

        row_rect.x = (float)(panel_left + 1);
        row_rect.y = (float)(start_y + row * row_height - 2);
        row_rect.w = (float)(window_width - panel_left - 2);
        row_rect.h = (float)(row_height - 1);

        if (last_white || last_black) {
            SDL_SetRenderDrawColor(ctx->renderer, 58, 52, 34, 180);
            SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);
            SDL_RenderFillRect(ctx->renderer, &row_rect);
            SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_NONE);
        }

        SDL_snprintf(turn_label, sizeof(turn_label), "%d.", turn);
        turn_tex_row = make_text_texture(ctx->renderer, s_coord_font, turn_label, turn_color);
        white_tex = make_text_texture(
            ctx->renderer,
            s_coord_font,
            white_move,
            last_white ? last_move_color : normal_move_color);
        black_tex = make_text_texture(
            ctx->renderer,
            s_coord_font,
            black_move,
            has_black ? (last_black ? last_move_color : normal_move_color) : placeholder_color);

        if (turn_tex_row) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(turn_tex_row, &tw, &th);
            dst.x = (float)x_turn;
            dst.y = (float)(start_y + row * row_height);
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(ctx->renderer, turn_tex_row, NULL, &dst);
            SDL_DestroyTexture(turn_tex_row);
        }

        if (white_tex) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(white_tex, &tw, &th);
            dst.x = (float)x_white;
            dst.y = (float)(start_y + row * row_height);
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(ctx->renderer, white_tex, NULL, &dst);
            SDL_DestroyTexture(white_tex);
        }

        if (black_tex) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(black_tex, &tw, &th);
            dst.x = (float)x_black;
            dst.y = (float)(start_y + row * row_height);
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(ctx->renderer, black_tex, NULL, &dst);
            SDL_DestroyTexture(black_tex);
        }
    }

    render_panel_buttons(ctx, panel_left, (int)panel_rect.w, window_height);
}

/* ------------------------------------------------------------------ */
/*  Resign / Draw buttons (bottom of panel)                            */
/* ------------------------------------------------------------------ */

/* Button geometry constants shared by rendering + hit-test */
#define BTN_HEIGHT     28
#define BTN_MARGIN      8
#define BTN_GAP         6

static void get_button_rects(int panel_left, int panel_width, int window_height,
                             SDL_FRect *left_btn, SDL_FRect *right_btn)
{
    float btn_y = (float)(window_height - BTN_HEIGHT - BTN_MARGIN);
    float col_sep = (float)panel_left + (float)panel_width * 0.52f;
    float half_gap = (float)BTN_GAP * 0.5f;

    left_btn->x  = (float)(panel_left + BTN_MARGIN);
    left_btn->y  = btn_y;
    left_btn->w  = col_sep - half_gap - left_btn->x;
    left_btn->h  = (float)BTN_HEIGHT;

    right_btn->x = col_sep + half_gap;
    right_btn->y = btn_y;
    right_btn->w = (float)(panel_left + panel_width - BTN_MARGIN) - right_btn->x;
    right_btn->h = (float)BTN_HEIGHT;
}

static void render_panel_button(SDL_Renderer *renderer, TTF_Font *font,
                                const SDL_FRect *rect, const char *label,
                                SDL_Color bg, SDL_Color fg)
{
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(renderer, rect);

    {
        SDL_Texture *tex = make_text_texture(renderer, font, label, fg);
        if (tex) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(tex, &tw, &th);
            dst.x = rect->x + (rect->w - tw) * 0.5f;
            dst.y = rect->y + (rect->h - th) * 0.5f;
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(renderer, tex, NULL, &dst);
            SDL_DestroyTexture(tex);
        }
    }
}

static void render_panel_buttons(AppContext *ctx, int panel_left, int panel_width, int window_height)
{
    SDL_FRect left_btn;
    SDL_FRect right_btn;
    const SDL_Color fg_normal  = {232, 232, 238, 255};
    const SDL_Color fg_dim     = {120, 120, 128, 255};
    bool game_over;
    bool offer_pending;
    bool offer_received;

    if (!ctx || !ctx->renderer || !s_coord_font) {
        return;
    }

    game_over      = (ctx->game_state.outcome != CHESS_OUTCOME_NONE);
    offer_pending  = ctx->network_session.draw_offer_pending;
    offer_received = ctx->network_session.draw_offer_received;

    get_button_rects(panel_left, panel_width, window_height, &left_btn, &right_btn);

    if (game_over) {
        /* Both grayed out */
        render_panel_button(ctx->renderer, s_coord_font, &left_btn,  "Resign", (SDL_Color){40, 40, 46, 255}, fg_dim);
        render_panel_button(ctx->renderer, s_coord_font, &right_btn, "Draw",   (SDL_Color){40, 40, 46, 255}, fg_dim);
    } else if (offer_received) {
        /* Accept (green) / Decline (red) */
        render_panel_button(ctx->renderer, s_coord_font, &left_btn,  "Accept",  (SDL_Color){30,  90, 40, 255}, fg_normal);
        render_panel_button(ctx->renderer, s_coord_font, &right_btn, "Decline", (SDL_Color){120, 30, 30, 255}, fg_normal);
    } else if (offer_pending) {
        /* Resign normal, Draw grayed "Pending..." */
        render_panel_button(ctx->renderer, s_coord_font, &left_btn,  "Resign",     (SDL_Color){70, 30, 30, 255}, fg_normal);
        render_panel_button(ctx->renderer, s_coord_font, &right_btn, "Pending...", (SDL_Color){40, 40, 46, 255}, fg_dim);
    } else {
        /* Normal: Resign / Draw */
        render_panel_button(ctx->renderer, s_coord_font, &left_btn,  "Resign", (SDL_Color){70, 30, 30, 255}, fg_normal);
        render_panel_button(ctx->renderer, s_coord_font, &right_btn, "Draw",   (SDL_Color){40, 60, 90, 255}, fg_normal);
    }
}

/* ------------------------------------------------------------------ */
/*  Button hit-test                                                    */
/* ------------------------------------------------------------------ */

ChessGameButton chess_ui_game_button_from_mouse(AppContext *ctx, int mouse_x, int mouse_y)
{
    SDL_FRect left_btn;
    SDL_FRect right_btn;
    int board_width;
    int window_width = 0;
    int window_height = 0;
    int panel_left;
    int panel_width;
    float mx;
    float my;
    bool game_over;
    bool offer_received;
    bool offer_pending;

    if (!ctx || !ctx->window || !ctx->network_session.game_started) {
        return CHESS_GAME_BUTTON_NONE;
    }

    mx = (float)mouse_x;
    my = (float)mouse_y;

    /* Check "Return to Lobby" button in the game-over overlay first */
    if (s_lobby_button_visible &&
        mx >= s_lobby_button_rect.x && mx < s_lobby_button_rect.x + s_lobby_button_rect.w &&
        my >= s_lobby_button_rect.y && my < s_lobby_button_rect.y + s_lobby_button_rect.h) {
        return CHESS_GAME_BUTTON_RETURN_LOBBY;
    }

    SDL_GetWindowSize(ctx->window, &window_width, &window_height);
    board_width  = chess_ui_board_width_for_window(window_width, true);
    panel_left   = board_width;
    panel_width  = window_width - board_width;

    if (panel_width <= 1 || mouse_x < panel_left) {
        return CHESS_GAME_BUTTON_NONE;
    }

    game_over      = (ctx->game_state.outcome != CHESS_OUTCOME_NONE);
    offer_received = ctx->network_session.draw_offer_received;
    offer_pending  = ctx->network_session.draw_offer_pending;

    if (game_over) {
        return CHESS_GAME_BUTTON_NONE;
    }

    get_button_rects(panel_left, panel_width, window_height, &left_btn, &right_btn);

    mx = (float)mouse_x;
    my = (float)mouse_y;

    if (mx >= left_btn.x && mx < left_btn.x + left_btn.w &&
        my >= left_btn.y && my < left_btn.y + left_btn.h) {
        return offer_received ? CHESS_GAME_BUTTON_ACCEPT_DRAW : CHESS_GAME_BUTTON_RESIGN;
    }

    if (mx >= right_btn.x && mx < right_btn.x + right_btn.w &&
        my >= right_btn.y && my < right_btn.y + right_btn.h) {
        if (offer_received) {
            return CHESS_GAME_BUTTON_DECLINE_DRAW;
        }
        if (!offer_pending) {
            return CHESS_GAME_BUTTON_DRAW;
        }
    }

    return CHESS_GAME_BUTTON_NONE;
}

/* ------------------------------------------------------------------ */
/*  Status message bar                                                 */
/* ------------------------------------------------------------------ */

static void render_status_message(AppContext *ctx, int width, int board_y)
{
    SDL_Texture *msg_tex;

    if (!ctx || !ctx->renderer || !ctx->status_message[0]) {
        return;
    }

    if (SDL_GetTicks() >= ctx->status_message_until_ms) {
        ctx->status_message[0] = '\0';
        return;
    }

    msg_tex = make_text_texture(
        ctx->renderer,
        s_lobby_font ? s_lobby_font : s_coord_font,
        ctx->status_message,
        (SDL_Color){255, 240, 130, 255}
    );
    if (msg_tex) {
        float tex_w = 0.0f;
        float tex_h = 0.0f;
        SDL_FRect bg_rect;
        SDL_FRect dst;

        SDL_GetTextureSize(msg_tex, &tex_w, &tex_h);
        bg_rect.w = tex_w + 16.0f;
        bg_rect.h = tex_h + 10.0f;
        bg_rect.x = ((float)width - bg_rect.w) * 0.5f;
        bg_rect.y = (float)board_y + 8.0f;
        dst.w = tex_w;
        dst.h = tex_h;
        dst.x = bg_rect.x + 8.0f;
        dst.y = bg_rect.y + 5.0f;

        SDL_SetRenderDrawColor(ctx->renderer, 35, 35, 35, 230);
        SDL_RenderFillRect(ctx->renderer, &bg_rect);
        SDL_SetRenderDrawColor(ctx->renderer, 215, 170, 70, 255);
        SDL_RenderRect(ctx->renderer, &bg_rect);
        SDL_RenderTexture(ctx->renderer, msg_tex, NULL, &dst);
        SDL_DestroyTexture(msg_tex);
    }
}

/* ------------------------------------------------------------------ */
/*  Player info panels (top / bottom)                                  */
/* ------------------------------------------------------------------ */

static void render_one_player_panel(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const ChessPeerInfo *peer,
    const ChessCapturedPieces *captured,
    SDL_FRect panel_rect,
    ChessPlayerColor player_color)
{
    const SDL_Color name_color = {240, 240, 240, 255};
    const SDL_Color host_color = {170, 170, 170, 255};
    float cursor_x = panel_rect.x + 8.0f;
    float text_y;
    /* Piece types to show as captures, in display order. */
    const ChessPiece white_order[] = {
        CHESS_PIECE_WHITE_PAWN, CHESS_PIECE_WHITE_KNIGHT,
        CHESS_PIECE_WHITE_BISHOP, CHESS_PIECE_WHITE_ROOK,
        CHESS_PIECE_WHITE_QUEEN
    };
    const ChessPiece black_order[] = {
        CHESS_PIECE_BLACK_PAWN, CHESS_PIECE_BLACK_KNIGHT,
        CHESS_PIECE_BLACK_BISHOP, CHESS_PIECE_BLACK_ROOK,
        CHESS_PIECE_BLACK_QUEEN
    };
    const ChessPiece *cap_order;
    size_t cap_count;
    size_t ci;

    if (!renderer || !font) {
        return;
    }

    /* Background */
    SDL_SetRenderDrawColor(renderer, 28, 28, 34, 255);
    SDL_RenderFillRect(renderer, &panel_rect);

    /* Separator line at the edge closest to the board */
    {
        SDL_FRect sep;
        sep.x = panel_rect.x;
        sep.y = panel_rect.y + panel_rect.h - 1.0f;
        sep.w = panel_rect.w;
        sep.h = 1.0f;
        if (panel_rect.y < 1.0f) {
            /* Top panel: separator at bottom */
            sep.y = panel_rect.y + panel_rect.h - 1.0f;
        } else {
            /* Bottom panel: separator at top */
            sep.y = panel_rect.y;
        }
        SDL_SetRenderDrawColor(renderer, 55, 55, 65, 255);
        SDL_RenderFillRect(renderer, &sep);
    }

    /* Player name: username bold + @hostname gray */
    if (peer && peer->username[0] != '\0') {
        SDL_Texture *name_tex = make_text_texture(renderer, font, peer->username, name_color);
        if (name_tex) {
            float nw = 0.0f;
            float nh = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(name_tex, &nw, &nh);
            text_y = panel_rect.y + (panel_rect.h - nh) * 0.5f;
            dst.x = cursor_x;
            dst.y = text_y;
            dst.w = nw;
            dst.h = nh;
            /* Bold trick: render twice with 1px offset */
            SDL_RenderTexture(renderer, name_tex, NULL, &dst);
            dst.x += 1.0f;
            SDL_RenderTexture(renderer, name_tex, NULL, &dst);
            cursor_x += nw + 2.0f;
            SDL_DestroyTexture(name_tex);
        }

        if (peer->hostname[0] != '\0') {
            char host_label[CHESS_PEER_HOSTNAME_MAX_LEN + 2];
            SDL_Texture *host_tex;
            SDL_snprintf(host_label, sizeof(host_label), "@%s", peer->hostname);
            host_tex = make_text_texture(renderer, font, host_label, host_color);
            if (host_tex) {
                float hw = 0.0f;
                float hh = 0.0f;
                SDL_FRect dst;
                SDL_GetTextureSize(host_tex, &hw, &hh);
                dst.x = cursor_x;
                dst.y = panel_rect.y + (panel_rect.h - hh) * 0.5f;
                dst.w = hw;
                dst.h = hh;
                SDL_RenderTexture(renderer, host_tex, NULL, &dst);
                cursor_x += hw;
                SDL_DestroyTexture(host_tex);
            }
        }
    } else if (peer && peer->profile_id[0] != '\0') {
        char id_label[16];
        SDL_Texture *id_tex;
        SDL_snprintf(id_label, sizeof(id_label), "%.8s...", peer->profile_id);
        id_tex = make_text_texture(renderer, font, id_label, name_color);
        if (id_tex) {
            float tw = 0.0f;
            float th = 0.0f;
            SDL_FRect dst;
            SDL_GetTextureSize(id_tex, &tw, &th);
            text_y = panel_rect.y + (panel_rect.h - th) * 0.5f;
            dst.x = cursor_x;
            dst.y = text_y;
            dst.w = tw;
            dst.h = th;
            SDL_RenderTexture(renderer, id_tex, NULL, &dst);
            cursor_x += tw;
            SDL_DestroyTexture(id_tex);
        }
    }

    /* Captured pieces: pieces captured BY this player (i.e. opponent's pieces) */
    if (!captured) {
        return;
    }

    /* This panel shows what opponent pieces this player has captured.
     * Player is white → show captured black pieces.
     * Player is black → show captured white pieces. */
    if (player_color == CHESS_COLOR_WHITE) {
        cap_order = black_order;
        cap_count = SDL_arraysize(black_order);
    } else {
        cap_order = white_order;
        cap_count = SDL_arraysize(white_order);
    }

    cursor_x += 12.0f;

    for (ci = 0; ci < cap_count; ++ci) {
        int piece_idx = (int)cap_order[ci];
        uint8_t n = captured->count[piece_idx];
        uint8_t k;
        if (n == 0) {
            continue;
        }
        for (k = 0; k < n; ++k) {
            SDL_Texture *tex = s_piece_textures[piece_idx];
            if (tex) {
                float tex_w = 0.0f;
                float tex_h = 0.0f;
                float scale;
                SDL_FRect dst;
                SDL_GetTextureSize(tex, &tex_w, &tex_h);
                if (tex_h > 0.0f) {
                    scale = 24.0f / tex_h;
                } else {
                    scale = 1.0f;
                }
                dst.w = tex_w * scale;
                dst.h = 24.0f;
                dst.x = cursor_x;
                dst.y = panel_rect.y + (panel_rect.h - dst.h) * 0.5f;
                if (piece_idx >= (int)CHESS_PIECE_WHITE_PAWN &&
                    piece_idx <= (int)CHESS_PIECE_WHITE_KING) {
                    SDL_FRect fill;
                    fill.w = dst.w * 0.52f;
                    fill.h = dst.h * 0.52f;
                    fill.x = dst.x + (dst.w - fill.w) * 0.5f;
                    fill.y = dst.y + (dst.h - fill.h) * 0.5f;
                    SDL_SetRenderDrawColor(renderer, 220, 215, 190, 255);
                    SDL_RenderFillRect(renderer, &fill);
                }
                SDL_RenderTexture(renderer, tex, NULL, &dst);
                cursor_x += dst.w * 0.55f; /* overlap for compact display */
            }
        }
        cursor_x += 4.0f; /* gap between piece types */
    }
}

static void render_player_panels(
    AppContext *ctx,
    int board_width,
    int window_height,
    int board_y,
    int board_height)
{
    const bool black_perspective = use_black_perspective(ctx->network_session.local_color);
    const ChessPeerInfo *top_peer;
    const ChessPeerInfo *bottom_peer;
    ChessPlayerColor top_color;
    ChessPlayerColor bottom_color;
    ChessCapturedPieces captured;
    TTF_Font *font = s_coord_font;
    SDL_FRect top_rect;
    SDL_FRect bottom_rect;

    if (!ctx || !ctx->renderer) {
        return;
    }

    chess_game_compute_captured(&ctx->game_state, &captured);

    if (black_perspective) {
        /* Black at bottom (local), white at top (opponent) */
        top_peer = &ctx->network_session.remote_peer;
        top_color = CHESS_COLOR_WHITE;
        bottom_peer = &ctx->network_session.local_peer;
        bottom_color = CHESS_COLOR_BLACK;
    } else {
        /* White at bottom (local), black at top (opponent) */
        top_peer = &ctx->network_session.remote_peer;
        top_color = CHESS_COLOR_BLACK;
        bottom_peer = &ctx->network_session.local_peer;
        bottom_color = CHESS_COLOR_WHITE;
    }

    top_rect.x = 0.0f;
    top_rect.y = 0.0f;
    top_rect.w = (float)board_width;
    top_rect.h = (float)CHESS_UI_PLAYER_PANEL_HEIGHT;

    bottom_rect.x = 0.0f;
    bottom_rect.y = (float)(board_y + board_height);
    bottom_rect.w = (float)board_width;
    bottom_rect.h = (float)CHESS_UI_PLAYER_PANEL_HEIGHT;

    render_one_player_panel(ctx->renderer, font, top_peer, &captured, top_rect, top_color);
    render_one_player_panel(ctx->renderer, font, bottom_peer, &captured, bottom_rect, bottom_color);
}

/* ------------------------------------------------------------------ */
/*  Public: full frame render                                          */
/* ------------------------------------------------------------------ */

void chess_ui_render_frame(AppContext *ctx)
{
    int width = 0;
    int board_width = 0;
    int height = 0;
    int board_y = 0;
    int board_height;
    bool hide_piece = false;
    int hidden_file = -1;
    int hidden_rank = -1;

    if (!ctx || !ctx->renderer || !ctx->window) {
        return;
    }

    SDL_GetWindowSize(ctx->window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network_session.game_started);
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->renderer);

    if (!ctx->network_session.game_started) {
        board_height = height;
        /* Update lobby hover state and cursor */
        {
            float mx = 0.0f;
            float my = 0.0f;
            int hovered;
            SDL_GetMouseState(&mx, &my);
            hovered = chess_lobby_find_clicked_peer(ctx->window, &ctx->lobby, (int)mx, (int)my);
            ctx->lobby.hovered_peer_idx = hovered;
            if (hovered >= 0 && ctx->cursor_pointer) {
                SDL_SetCursor(ctx->cursor_pointer);
            } else if (ctx->cursor_default) {
                SDL_SetCursor(ctx->cursor_default);
            }
        }
        chess_lobby_render(ctx->renderer, width, height, &ctx->lobby, s_lobby_font ? s_lobby_font : s_coord_font);
    } else {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
        if (board_height < 0) {
            board_height = height;
            board_y = 0;
        }

        if (ctx->drag_active) {
            hide_piece = true;
            hidden_file = ctx->drag_from_file;
            hidden_rank = ctx->drag_from_rank;
        } else if (ctx->remote_move_anim_active) {
            hide_piece = true;
            hidden_file = ctx->remote_move_to_file;
            hidden_rank = ctx->remote_move_to_rank;
        }

        render_player_panels(ctx, board_width, height, board_y, board_height);
        render_board(ctx->renderer, board_width, board_y, board_height);
        render_game_overlay(
            ctx->renderer,
            board_width,
            board_y,
            board_height,
            &ctx->game_state,
            ctx->network_session.local_color,
            hide_piece,
            hidden_file,
            hidden_rank);
        render_promotion_overlay(ctx, board_width, board_y, board_height);
        render_remote_move_animation(ctx, board_width, board_y, board_height);
        render_drag_preview(ctx, board_width, board_height);
        render_board_coordinates(ctx->renderer, board_width, board_y, board_height, ctx->network_session.local_color);
        render_game_over_banner(ctx, board_width, board_y, board_height);
        render_move_history_panel(ctx, width, height, board_width);
    }

    render_status_message(ctx, board_width, board_y);

    SDL_RenderPresent(ctx->renderer);
}
