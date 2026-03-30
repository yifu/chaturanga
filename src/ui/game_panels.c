/**
 * Game UI — side-panel rendering and player info panels.
 *
 * Extracted from ui/game.c to keep each module under ~800 lines.
 * Contains:
 *   - Move history panel (right side)
 *   - Resign / Draw buttons at the bottom of the panel
 *   - Player info panels (top / bottom of the board)
 *   - Button hit-testing (chess_ui_game_button_from_mouse)
 */
#include "game_internal.h"

#include <SDL3_ttf/SDL_ttf.h>

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

    if (!ctx || !ctx->win.renderer || !s_coord_font) {
        return;
    }

    game_over      = (ctx->game.game_state.outcome != CHESS_OUTCOME_NONE);
    offer_pending  = ctx->network.network_session.draw_offer_pending;
    offer_received = ctx->network.network_session.draw_offer_received;

    get_button_rects(panel_left, panel_width, window_height, &left_btn, &right_btn);

    if (game_over) {
        /* Both grayed out */
        render_panel_button(ctx->win.renderer, s_coord_font, &left_btn,  "Resign", (SDL_Color){40, 40, 46, 255}, fg_dim);
        render_panel_button(ctx->win.renderer, s_coord_font, &right_btn, "Draw",   (SDL_Color){40, 40, 46, 255}, fg_dim);
    } else if (offer_received) {
        /* Accept (green) / Decline (red) */
        render_panel_button(ctx->win.renderer, s_coord_font, &left_btn,  "Accept",  (SDL_Color){30,  90, 40, 255}, fg_normal);
        render_panel_button(ctx->win.renderer, s_coord_font, &right_btn, "Decline", (SDL_Color){120, 30, 30, 255}, fg_normal);
    } else if (offer_pending) {
        /* Resign normal, Draw grayed "Pending..." */
        render_panel_button(ctx->win.renderer, s_coord_font, &left_btn,  "Resign",     (SDL_Color){70, 30, 30, 255}, fg_normal);
        render_panel_button(ctx->win.renderer, s_coord_font, &right_btn, "Pending...", (SDL_Color){40, 40, 46, 255}, fg_dim);
    } else {
        /* Normal: Resign / Draw */
        render_panel_button(ctx->win.renderer, s_coord_font, &left_btn,  "Resign", (SDL_Color){70, 30, 30, 255}, fg_normal);
        render_panel_button(ctx->win.renderer, s_coord_font, &right_btn, "Draw",   (SDL_Color){40, 60, 90, 255}, fg_normal);
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

    if (!ctx || !ctx->win.window || !ctx->network.network_session.game_started) {
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

    SDL_GetWindowSize(ctx->win.window, &window_width, &window_height);
    board_width  = chess_ui_board_width_for_window(window_width, true);
    panel_left   = board_width;
    panel_width  = window_width - board_width;

    if (panel_width <= 1 || mouse_x < panel_left) {
        return CHESS_GAME_BUTTON_NONE;
    }

    game_over      = (ctx->game.game_state.outcome != CHESS_OUTCOME_NONE);
    offer_received = ctx->network.network_session.draw_offer_received;
    offer_pending  = ctx->network.network_session.draw_offer_pending;

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
/*  Move history panel                                                 */
/* ------------------------------------------------------------------ */

void chess_ui_render_move_history_panel(AppContext *ctx, int window_width, int window_height, int board_width)
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

    if (!ctx || !ctx->win.renderer || !s_coord_font || !ctx->network.network_session.game_started) {
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
    current_turn = ((int)ctx->game.move_history_count / 2) + 1;

    SDL_SetRenderDrawColor(ctx->win.renderer, 20, 20, 24, 255);
    SDL_RenderFillRect(ctx->win.renderer, &panel_rect);

    border_rect.x = (float)board_width;
    border_rect.y = 0.0f;
    border_rect.w = 1.0f;
    border_rect.h = (float)window_height;
    SDL_SetRenderDrawColor(ctx->win.renderer, 84, 84, 94, 255);
    SDL_RenderFillRect(ctx->win.renderer, &border_rect);

    header_rect.x = (float)(panel_left + 1);
    header_rect.y = 0.0f;
    header_rect.w = (float)(window_width - panel_left - 1);
    header_rect.h = 56.0f;
    SDL_SetRenderDrawColor(ctx->win.renderer, 28, 28, 34, 255);
    SDL_RenderFillRect(ctx->win.renderer, &header_rect);

    title_tex = make_text_texture(ctx->win.renderer, s_coord_font, "Moves (Ctrl/Cmd+C)", (SDL_Color){232, 232, 238, 255});
    turn_tex = NULL;
    white_header_tex = make_text_texture(ctx->win.renderer, s_coord_font, "White", white_black_header_color);
    black_header_tex = make_text_texture(ctx->win.renderer, s_coord_font, "Black", white_black_header_color);

    {
        char turn_label[32];
        SDL_snprintf(turn_label, sizeof(turn_label), "Turn %d", current_turn);
        turn_tex = make_text_texture(ctx->win.renderer, s_coord_font, turn_label, (SDL_Color){198, 198, 206, 255});
    }

    if (title_tex) {
        SDL_FRect title_dst;
        SDL_GetTextureSize(title_tex, &title_w, &title_h);
        title_dst.x = (float)panel_left + 10.0f;
        title_dst.y = 10.0f;
        title_dst.w = title_w;
        title_dst.h = title_h;
        SDL_RenderTexture(ctx->win.renderer, title_tex, NULL, &title_dst);
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
        SDL_RenderTexture(ctx->win.renderer, turn_tex, NULL, &turn_dst);
        SDL_DestroyTexture(turn_tex);
    }

    if (white_header_tex) {
        SDL_FRect dst;
        SDL_GetTextureSize(white_header_tex, &white_header_w, &white_header_h);
        dst.x = (float)x_white;
        dst.y = 40.0f;
        dst.w = white_header_w;
        dst.h = white_header_h;
        SDL_RenderTexture(ctx->win.renderer, white_header_tex, NULL, &dst);
        SDL_DestroyTexture(white_header_tex);
    }

    if (black_header_tex) {
        SDL_FRect dst;
        SDL_GetTextureSize(black_header_tex, &black_header_w, &black_header_h);
        dst.x = (float)x_black;
        dst.y = 40.0f;
        dst.w = black_header_w;
        dst.h = black_header_h;
        SDL_RenderTexture(ctx->win.renderer, black_header_tex, NULL, &dst);
        SDL_DestroyTexture(black_header_tex);
    }

    SDL_SetRenderDrawColor(ctx->win.renderer, 74, 74, 84, 255);
    SDL_RenderLine(ctx->win.renderer, (float)(panel_left + 1), 56.0f, (float)panel_right, 56.0f);
    SDL_RenderLine(ctx->win.renderer, (float)col_sep_x, 38.0f, (float)col_sep_x, (float)window_height);

    max_rows = (window_height - start_y - 8) / row_height;
    if (max_rows <= 0) {
        render_panel_buttons(ctx, panel_left, (int)panel_rect.w, window_height);
        return;
    }

    total_turns = ((int)ctx->game.move_history_count + 1) / 2;
    if (total_turns <= 0) {
        SDL_Texture *empty_tex = make_text_texture(
            ctx->win.renderer,
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
            SDL_RenderTexture(ctx->win.renderer, empty_tex, NULL, &dst);
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
        bool has_white = white_idx < (int)ctx->game.move_history_count;
        bool has_black = black_idx < (int)ctx->game.move_history_count;
        bool last_white = has_white && white_idx == ((int)ctx->game.move_history_count - 1);
        bool last_black = has_black && black_idx == ((int)ctx->game.move_history_count - 1);
        const char *white_move = has_white ? ctx->game.move_history[white_idx] : "";
        const char *black_move = has_black ? ctx->game.move_history[black_idx] : "...";
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
            SDL_SetRenderDrawColor(ctx->win.renderer, 58, 52, 34, 180);
            SDL_SetRenderDrawBlendMode(ctx->win.renderer, SDL_BLENDMODE_BLEND);
            SDL_RenderFillRect(ctx->win.renderer, &row_rect);
            SDL_SetRenderDrawBlendMode(ctx->win.renderer, SDL_BLENDMODE_NONE);
        }

        SDL_snprintf(turn_label, sizeof(turn_label), "%d.", turn);
        turn_tex_row = make_text_texture(ctx->win.renderer, s_coord_font, turn_label, turn_color);
        white_tex = make_text_texture(
            ctx->win.renderer,
            s_coord_font,
            white_move,
            last_white ? last_move_color : normal_move_color);
        black_tex = make_text_texture(
            ctx->win.renderer,
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
            SDL_RenderTexture(ctx->win.renderer, turn_tex_row, NULL, &dst);
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
            SDL_RenderTexture(ctx->win.renderer, white_tex, NULL, &dst);
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
            SDL_RenderTexture(ctx->win.renderer, black_tex, NULL, &dst);
            SDL_DestroyTexture(black_tex);
        }
    }

    render_panel_buttons(ctx, panel_left, (int)panel_rect.w, window_height);
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

    /* Record cursor origin for capture animation targeting */
    if (panel_rect.y < 1.0f) {
        s_cap_cursor_start_top = cursor_x;
    } else {
        s_cap_cursor_start_bottom = cursor_x;
    }

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
                /* White silhouette halo behind dark pieces on dark panel */
                if (piece_idx >= (int)CHESS_PIECE_BLACK_PAWN &&
                    piece_idx <= (int)CHESS_PIECE_BLACK_KING &&
                    s_piece_silhouettes[piece_idx]) {
                    static const float offsets[][2] = {
                        {-2, -2}, {-1, -2}, { 0, -2}, { 1, -2}, { 2, -2},
                        {-2, -1},                               { 2, -1},
                        {-2,  0},                               { 2,  0},
                        {-2,  1},                               { 2,  1},
                        {-2,  2}, {-1,  2}, { 0,  2}, { 1,  2}, { 2,  2},
                    };
                    size_t oi;
                    for (oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); ++oi) {
                        SDL_FRect halo = dst;
                        halo.x += offsets[oi][0];
                        halo.y += offsets[oi][1];
                        SDL_RenderTexture(renderer, s_piece_silhouettes[piece_idx], NULL, &halo);
                    }
                }
                SDL_RenderTexture(renderer, tex, NULL, &dst);
                cursor_x += dst.w * 0.55f; /* overlap for compact display */
            }
        }
        cursor_x += 4.0f; /* gap between piece types */
    }
}

void chess_ui_render_player_panels(
    AppContext *ctx,
    int board_width,
    int window_height,
    int board_y,
    int board_height)
{
    const bool black_perspective = use_black_perspective(ctx->network.network_session.local_color);
    const ChessPeerInfo *top_peer;
    const ChessPeerInfo *bottom_peer;
    ChessPlayerColor top_color;
    ChessPlayerColor bottom_color;
    ChessCapturedPieces captured;
    TTF_Font *font = s_coord_font;
    SDL_FRect top_rect;
    SDL_FRect bottom_rect;

    if (!ctx || !ctx->win.renderer) {
        return;
    }

    chess_game_compute_captured(&ctx->game.game_state, &captured);

    if (black_perspective) {
        /* Black at bottom (local), white at top (opponent) */
        top_peer = &ctx->network.network_session.remote_peer;
        top_color = CHESS_COLOR_WHITE;
        bottom_peer = &ctx->network.network_session.local_peer;
        bottom_color = CHESS_COLOR_BLACK;
    } else {
        /* White at bottom (local), black at top (opponent) */
        top_peer = &ctx->network.network_session.remote_peer;
        top_color = CHESS_COLOR_BLACK;
        bottom_peer = &ctx->network.network_session.local_peer;
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

    render_one_player_panel(ctx->win.renderer, font, top_peer, &captured, top_rect, top_color);
    render_one_player_panel(ctx->win.renderer, font, bottom_peer, &captured, bottom_rect, bottom_color);
}
