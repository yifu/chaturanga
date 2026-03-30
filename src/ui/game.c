#include "game_internal.h"
#include "chess_app/ui_lobby.h"
#include "chess_app/render_board.h"

#include <SDL3_ttf/SDL_ttf.h>

/* ------------------------------------------------------------------ */
/*  Shared module-level state (owned here, extern in game_internal.h)  */
/* ------------------------------------------------------------------ */

SDL_FRect s_lobby_button_rect = {0};
bool      s_lobby_button_visible = false;
float     s_cap_cursor_start_top = 0.0f;
float     s_cap_cursor_start_bottom = 0.0f;

/* ------------------------------------------------------------------ */
/*  Promotion helpers                                                  */
/* ------------------------------------------------------------------ */

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

    if (!ctx || !ctx->win.window || !out_file || !out_rank) {
        return false;
    }

    SDL_GetWindowSize(ctx->win.window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network.network_session.game_started);
    if (mouse_x < 0 || mouse_x >= board_width) {
        return false;
    }

    if (ctx->network.network_session.game_started) {
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
    black_perspective = use_black_perspective(ctx->network.network_session.local_color);
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

    if (!ctx || !out_rect || !ctx->ui.drag.promotion_pending) {
        return false;
    }

    black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    cell_w = (float)width / (float)CHESS_BOARD_SIZE;
    cell_h = (float)board_height / (float)CHESS_BOARD_SIZE;
    screen_file = board_to_screen_index(ctx->ui.drag.promotion_to_file, black_perspective);
    screen_rank = board_to_screen_index(ctx->ui.drag.promotion_to_rank, black_perspective);

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

    if (!ctx || !ctx->win.window || !ctx->ui.drag.promotion_pending) {
        return CHESS_PROMOTION_NONE;
    }

    SDL_GetWindowSize(ctx->win.window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network.network_session.game_started);
    if (ctx->network.network_session.game_started) {
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

    if (!ctx || !ctx->win.renderer || !ctx->ui.drag.drag_active ||
        ctx->ui.drag.drag_piece <= CHESS_PIECE_EMPTY || ctx->ui.drag.drag_piece >= CHESS_PIECE_COUNT) {
        return;
    }

    {
        SDL_Texture *tex = s_piece_textures[(int)ctx->ui.drag.drag_piece];
        if (tex) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            SDL_FRect dst;

            SDL_GetTextureSize(tex, &tex_w, &tex_h);
            dst.x = (float)ctx->ui.drag.drag_mouse_x - tex_w * 0.5f;
            dst.y = (float)ctx->ui.drag.drag_mouse_y - tex_h * 0.5f;
            dst.w = tex_w;
            dst.h = tex_h;
            SDL_RenderTexture(ctx->win.renderer, tex, NULL, &dst);
        } else {
            SDL_FRect piece_rect = {
                (float)ctx->ui.drag.drag_mouse_x - cell_w * 0.25f,
                (float)ctx->ui.drag.drag_mouse_y - cell_h * 0.25f,
                cell_w * 0.5f,
                cell_h * 0.5f
            };

            if ((int)ctx->ui.drag.drag_piece < (int)CHESS_PIECE_BLACK_PAWN) {
                SDL_SetRenderDrawColor(ctx->win.renderer, 245, 245, 245, 255);
            } else {
                SDL_SetRenderDrawColor(ctx->win.renderer, 25, 25, 25, 255);
            }
            SDL_RenderFillRect(ctx->win.renderer, &piece_rect);
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

    if (!ctx || !ctx->win.renderer || !ctx->ui.drag.promotion_pending) {
        return;
    }

    for (i = 0; i < SDL_arraysize(choices); ++i) {
        SDL_FRect rect;
        ChessPiece piece;

        if (!promotion_choice_rect(ctx, width, board_y, board_height, choices[i], &rect)) {
            continue;
        }

        SDL_SetRenderDrawBlendMode(ctx->win.renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ctx->win.renderer, 28, 28, 28, 225);
        SDL_RenderFillRect(ctx->win.renderer, &rect);
        SDL_SetRenderDrawColor(ctx->win.renderer, 220, 185, 80, 255);
        SDL_RenderRect(ctx->win.renderer, &rect);

        piece = promotion_choice_piece(ctx->network.network_session.local_color, choices[i]);
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
            SDL_RenderTexture(ctx->win.renderer, s_piece_textures[(int)piece], NULL, &dst);
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

    if (!ctx || !ctx->win.renderer || ctx->game.game_state.outcome == CHESS_OUTCOME_NONE) {
        s_lobby_button_visible = false;
        return;
    }

    headline = NULL;
    subline = NULL;
    switch (ctx->game.game_state.outcome) {
    case CHESS_OUTCOME_CHECKMATE_WHITE_WINS:
        headline = "Checkmate";
        subline = (ctx->network.network_session.local_color == CHESS_COLOR_WHITE) ? "You win!" : "Opponent wins";
        break;
    case CHESS_OUTCOME_CHECKMATE_BLACK_WINS:
        headline = "Checkmate";
        subline = (ctx->network.network_session.local_color == CHESS_COLOR_BLACK) ? "You win!" : "Opponent wins";
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
        subline = (ctx->network.network_session.local_color == CHESS_COLOR_WHITE) ? "You resigned" : "Opponent resigned";
        break;
    case CHESS_OUTCOME_BLACK_RESIGNED:
        headline = "Resignation";
        subline = (ctx->network.network_session.local_color == CHESS_COLOR_BLACK) ? "You resigned" : "Opponent resigned";
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
        SDL_SetRenderDrawBlendMode(ctx->win.renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ctx->win.renderer, 0, 0, 0, 160);
        SDL_RenderFillRect(ctx->win.renderer, &overlay);
        SDL_SetRenderDrawBlendMode(ctx->win.renderer, SDL_BLENDMODE_NONE);
    }

    hw = 0.0f;
    hh = 0.0f;
    sw = 0.0f;
    sh = 0.0f;
    headline_tex = make_text_texture(
        ctx->win.renderer,
        s_lobby_font ? s_lobby_font : s_coord_font,
        headline,
        (SDL_Color){ 255, 255, 255, 255 }
    );
    subline_tex = make_text_texture(
        ctx->win.renderer,
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
            ctx->win.renderer, s_coord_font, "Return to Lobby",
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

        SDL_SetRenderDrawColor(ctx->win.renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(ctx->win.renderer, &box);
        SDL_SetRenderDrawColor(ctx->win.renderer, 200, 160, 60, 255);
        SDL_RenderRect(ctx->win.renderer, &box);

        if (headline_tex) {
            dst.x = box.x + (box.w - hw) * 0.5f;
            dst.y = box.y + pad;
            dst.w = hw;
            dst.h = hh;
            SDL_RenderTexture(ctx->win.renderer, headline_tex, NULL, &dst);
            SDL_DestroyTexture(headline_tex);
        }
        if (subline_tex) {
            dst.x = box.x + (box.w - sw) * 0.5f;
            dst.y = box.y + pad + hh + gap;
            dst.w = sw;
            dst.h = sh;
            SDL_RenderTexture(ctx->win.renderer, subline_tex, NULL, &dst);
            SDL_DestroyTexture(subline_tex);
        }

        /* "Return to Lobby" button */
        {
            const float btn_w = box_w - pad * 2.0f;

            btn_rect.x = box.x + pad;
            btn_rect.y = box.y + pad + hh + gap + sh + btn_gap;
            btn_rect.w = btn_w;
            btn_rect.h = btn_h;

            SDL_SetRenderDrawColor(ctx->win.renderer, 60, 60, 70, 255);
            SDL_RenderFillRect(ctx->win.renderer, &btn_rect);
            SDL_SetRenderDrawColor(ctx->win.renderer, 150, 150, 160, 255);
            SDL_RenderRect(ctx->win.renderer, &btn_rect);

            if (btn_tex) {
                SDL_FRect bdst;
                bdst.x = btn_rect.x + (btn_rect.w - btn_text_w) * 0.5f;
                bdst.y = btn_rect.y + (btn_rect.h - btn_text_h) * 0.5f;
                bdst.w = btn_text_w;
                bdst.h = btn_text_h;
                SDL_RenderTexture(ctx->win.renderer, btn_tex, NULL, &bdst);
                SDL_DestroyTexture(btn_tex);
            }

            s_lobby_button_rect = btn_rect;
            s_lobby_button_visible = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Status message bar                                                 */
/* ------------------------------------------------------------------ */

static void render_status_message(AppContext *ctx, int width, int board_y)
{
    SDL_Texture *msg_tex;

    if (!ctx || !ctx->win.renderer || !ctx->ui.status_message[0]) {
        return;
    }

    if (SDL_GetTicks() >= ctx->ui.status_message_until_ms) {
        ctx->ui.status_message[0] = '\0';
        return;
    }

    msg_tex = make_text_texture(
        ctx->win.renderer,
        s_lobby_font ? s_lobby_font : s_coord_font,
        ctx->ui.status_message,
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

        SDL_SetRenderDrawColor(ctx->win.renderer, 35, 35, 35, 230);
        SDL_RenderFillRect(ctx->win.renderer, &bg_rect);
        SDL_SetRenderDrawColor(ctx->win.renderer, 215, 170, 70, 255);
        SDL_RenderRect(ctx->win.renderer, &bg_rect);
        SDL_RenderTexture(ctx->win.renderer, msg_tex, NULL, &dst);
        SDL_DestroyTexture(msg_tex);
    }
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

    if (!ctx || !ctx->win.renderer || !ctx->win.window) {
        return;
    }

    SDL_GetWindowSize(ctx->win.window, &width, &height);
    board_width = chess_ui_board_width_for_window(width, ctx->network.network_session.game_started);
    SDL_SetRenderDrawColor(ctx->win.renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->win.renderer);

    if (!ctx->network.network_session.game_started) {
        board_height = height;
        /* Update lobby hover state and cursor */
        {
            float mx = 0.0f;
            float my = 0.0f;
            int hovered;
            SDL_GetMouseState(&mx, &my);
            hovered = chess_lobby_find_clicked_peer(ctx->win.window, &ctx->game.lobby, (int)mx, (int)my);
            ctx->game.lobby.hovered_peer_idx = hovered;
            if (hovered >= 0 && ctx->win.cursor_pointer) {
                SDL_SetCursor(ctx->win.cursor_pointer);
            } else if (ctx->win.cursor_default) {
                SDL_SetCursor(ctx->win.cursor_default);
            }
        }
        chess_lobby_render(ctx->win.renderer, width, height, &ctx->game.lobby, s_lobby_font ? s_lobby_font : s_coord_font);
    } else {
        board_y = CHESS_UI_PLAYER_PANEL_HEIGHT;
        board_height = height - 2 * CHESS_UI_PLAYER_PANEL_HEIGHT;
        if (board_height < 0) {
            board_height = height;
            board_y = 0;
        }

        if (ctx->ui.drag.drag_active) {
            hide_piece = true;
            hidden_file = ctx->ui.drag.drag_from_file;
            hidden_rank = ctx->ui.drag.drag_from_rank;
        } else if (ctx->ui.remote_move_anim.active) {
            hide_piece = true;
            hidden_file = ctx->ui.remote_move_anim.to_file;
            hidden_rank = ctx->ui.remote_move_anim.to_rank;
        }

        chess_ui_render_player_panels(ctx, board_width, height, board_y, board_height);
        render_board(ctx->win.renderer, board_width, board_y, board_height);
        render_game_overlay(
            ctx->win.renderer,
            board_width,
            board_y,
            board_height,
            &ctx->game.game_state,
            ctx->network.network_session.local_color,
            hide_piece,
            hidden_file,
            hidden_rank);
        render_promotion_overlay(ctx, board_width, board_y, board_height);
        chess_ui_render_remote_move_animation(ctx, board_width, board_y, board_height);
        chess_ui_render_capture_animation(ctx, board_width, board_y, board_height);
        render_drag_preview(ctx, board_width, board_height);
        render_board_coordinates(ctx->win.renderer, board_width, board_y, board_height, ctx->network.network_session.local_color);
        render_game_over_banner(ctx, board_width, board_y, board_height);
        chess_ui_render_move_history_panel(ctx, width, height, board_width);
    }

    render_status_message(ctx, board_width, board_y);

    SDL_RenderPresent(ctx->win.renderer);
}
