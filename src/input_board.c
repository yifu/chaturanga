/**
 * Board interaction: mouse handling and local move submission.
 *
 * Split from input_handler.c for focused module size.
 */
#include "input_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/game_state.h"
#include "chess_app/network_session.h"
#include "chess_app/persistence.h"
#include "chess_app/transport.h"
#include "chess_app/ui_game.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* Declared in game_state_internal.h */
extern bool chess_gs_is_king_in_check(const ChessGameState *state, ChessPlayerColor color);

/* ---------- move sending ---------- */

bool chess_input_try_send_local_move(AppContext *ctx, int to_file, int to_rank, uint8_t promotion)
{
    ChessMovePayload move;
    char notation[24];
    bool notation_ready = false;

    if (!ctx || transport_get_fd(&ctx->network.transport.base) < 0) {
        return false;
    }

    if (ctx->game.game_state.outcome != CHESS_OUTCOME_NONE) {
        return false;
    }

    if (promotion == CHESS_PROMOTION_NONE &&
        chess_game_local_move_requires_promotion(
            &ctx->game.game_state,
            ctx->network.network_session.local_color,
            to_file,
            to_rank)) {
        ctx->ui.drag.promotion_pending = true;
        ctx->ui.drag.promotion_to_file = to_file;
        ctx->ui.drag.promotion_to_rank = to_rank;
        app_set_status_message(ctx, "Promotion: press Q (queen), R (rook), B (bishop), N (knight)", 30000u);
        return false;
    }

    if (ctx->game.game_state.has_selection &&
        chess_move_format_algebraic_notation(
            &ctx->game.game_state,
            ctx->game.game_state.selected_file,
            ctx->game.game_state.selected_rank,
            to_file,
            to_rank,
            promotion,
            notation,
            sizeof(notation))) {
        notation_ready = true;
    }

    /* Detect capture before the move modifies the board */
    {
        ChessPiece victim = CHESS_PIECE_EMPTY;
        int victim_file = to_file;
        int victim_rank = to_rank;

        if (ctx->game.game_state.has_selection) {
            victim = chess_game_get_piece(&ctx->game.game_state, to_file, to_rank);
            if (victim == CHESS_PIECE_EMPTY) {
                /* Check for en passant: pawn moving diagonally to empty square */
                ChessPiece mover = chess_game_get_piece(
                    &ctx->game.game_state,
                    ctx->game.game_state.selected_file,
                    ctx->game.game_state.selected_rank);
                if ((mover == CHESS_PIECE_WHITE_PAWN || mover == CHESS_PIECE_BLACK_PAWN) &&
                    to_file != ctx->game.game_state.selected_file) {
                    victim_rank = ctx->game.game_state.selected_rank;
                    victim = chess_game_get_piece(&ctx->game.game_state, to_file, victim_rank);
                }
            }
        }

        if (!chess_game_try_local_move(
                &ctx->game.game_state,
                ctx->network.network_session.local_color,
                to_file,
                to_rank,
                promotion,
                &move)) {
            return false;
        }

        if (victim != CHESS_PIECE_EMPTY) {
            chess_ui_start_capture_animation(ctx, victim, victim_file, victim_rank);
        }

        /* Tilt on checkmate, bounce on check */
        {
            ChessGameOutcome outcome = ctx->game.game_state.outcome;
            if (outcome == CHESS_OUTCOME_CHECKMATE_WHITE_WINS ||
                outcome == CHESS_OUTCOME_CHECKMATE_BLACK_WINS) {
                ChessPlayerColor loser = (outcome == CHESS_OUTCOME_CHECKMATE_WHITE_WINS)
                    ? CHESS_COLOR_BLACK : CHESS_COLOR_WHITE;
                ChessPiece king_target = (loser == CHESS_COLOR_WHITE)
                    ? CHESS_PIECE_WHITE_KING : CHESS_PIECE_BLACK_KING;
                int r, f;
                for (r = 0; r < CHESS_BOARD_SIZE; ++r) {
                    for (f = 0; f < CHESS_BOARD_SIZE; ++f) {
                        if (chess_game_get_piece(&ctx->game.game_state, f, r) == king_target) {
                            chess_ui_start_king_tilt_animation(ctx, f, r);
                            r = CHESS_BOARD_SIZE;
                            break;
                        }
                    }
                }
            } else {
                ChessPlayerColor opponent = ctx->game.game_state.side_to_move;
                if (chess_gs_is_king_in_check(&ctx->game.game_state, opponent)) {
                    ChessPiece king_target = (opponent == CHESS_COLOR_WHITE)
                        ? CHESS_PIECE_WHITE_KING : CHESS_PIECE_BLACK_KING;
                    int r, f;
                    for (r = 0; r < CHESS_BOARD_SIZE; ++r) {
                        for (f = 0; f < CHESS_BOARD_SIZE; ++f) {
                            if (chess_game_get_piece(&ctx->game.game_state, f, r) == king_target) {
                                chess_ui_start_king_bounce_animation(ctx, f, r);
                                r = CHESS_BOARD_SIZE;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (notation_ready) {
        app_append_move_history(ctx, notation);
    }

    if (ctx->network.network_session.role == CHESS_ROLE_SERVER) {
        (void)chess_persist_save_match_snapshot(ctx);
    }

    ctx->ui.drag.promotion_pending = false;
    ctx->ui.drag.promotion_to_file = -1;
    ctx->ui.drag.promotion_to_rank = -1;
    ctx->ui.status_message[0] = '\0';
    ctx->ui.status_message_until_ms = 0;

    /* Playing a move implicitly declines any pending draw offer from opponent. */
    if (ctx->network.network_session.draw_offer_received) {
        ctx->network.network_session.draw_offer_received = false;
        transport_send_packet(&ctx->network.transport.base, CHESS_MSG_DRAW_DECLINE,
                              ctx->protocol.move_sequence, NULL, 0u);
    }

    /* Server: deduct time and send MOVE + TIME_SYNC atomically via writev */
    if (ctx->network.network_session.role == CHESS_ROLE_SERVER &&
        ctx->game.turn_started_at_ms > 0) {
        uint64_t now = SDL_GetTicks();
        uint64_t elapsed = now - ctx->game.turn_started_at_ms;
        uint32_t *mover_remaining = (ctx->network.network_session.local_color == CHESS_COLOR_WHITE)
            ? &ctx->game.white_remaining_ms
            : &ctx->game.black_remaining_ms;
        ChessTimeSyncPayload ts;

        if (elapsed >= *mover_remaining) {
            *mover_remaining = 0;
        } else {
            *mover_remaining -= (uint32_t)elapsed;
        }
        ctx->game.turn_started_at_ms = now;
        ctx->game.last_clock_sync_ticks = now;

        ts.white_remaining_ms = ctx->game.white_remaining_ms;
        ts.black_remaining_ms = ctx->game.black_remaining_ms;

        if (!transport_send_move_with_time_sync(
                &ctx->network.transport.base,
                ctx->protocol.move_sequence++,
                &move, &ts)) {
            SDL_Log("NET: failed to send MOVE+TIME_SYNC packet, closing connection");
            app_handle_peer_disconnect(ctx, "failed to send MOVE packet");
            return false;
        }
    } else {
        if (!transport_send_packet(
                &ctx->network.transport.base,
                CHESS_MSG_MOVE,
                ctx->protocol.move_sequence++,
                &move,
                (uint32_t)sizeof(move))) {
            SDL_Log("NET: failed to send MOVE packet, closing connection");
            app_handle_peer_disconnect(ctx, "failed to send MOVE packet");
            return false;
        }
    }

    SDL_Log(
        "GAME: sent local move (%u,%u) -> (%u,%u)",
        (unsigned)move.from_file,
        (unsigned)move.from_rank,
        (unsigned)move.to_file,
        (unsigned)move.to_rank
    );

    return true;
}

/* ---------- board mouse handling ---------- */

void chess_input_handle_board_mouse_down(AppContext *ctx, int mouse_x, int mouse_y)
{
    int file;
    int rank;

    if (!ctx || transport_get_fd(&ctx->network.transport.base) < 0) {
        return;
    }

    if (ctx->game.game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    if (ctx->ui.drag.promotion_pending) {
        uint8_t promotion = chess_ui_promotion_from_mouse(ctx, mouse_x, mouse_y);
        if (promotion != CHESS_PROMOTION_NONE) {
            (void)chess_input_try_send_local_move(ctx, ctx->ui.drag.promotion_to_file, ctx->ui.drag.promotion_to_rank, promotion);
        }
        return;
    }

    if (!chess_ui_screen_to_board_square(ctx, mouse_x, mouse_y, &file, &rank)) {
        return;
    }

    {
        bool was_selected = ctx->game.game_state.has_selection
            && ctx->game.game_state.selected_file == file
            && ctx->game.game_state.selected_rank == rank;

        if (chess_game_select_local_piece(&ctx->game.game_state, ctx->network.network_session.local_color, file, rank)) {
            ctx->ui.drag.drag_active = true;
            ctx->ui.drag.drag_piece = chess_game_get_piece(&ctx->game.game_state, file, rank);
            ctx->ui.drag.drag_from_file = file;
            ctx->ui.drag.drag_from_rank = rank;
            ctx->ui.drag.drag_mouse_x = mouse_x;
            ctx->ui.drag.drag_mouse_y = mouse_y;
            ctx->ui.drag.was_already_selected = was_selected;
            return;
        }
    }

    if (ctx->game.game_state.has_selection) {
        (void)chess_input_try_send_local_move(ctx, file, rank, CHESS_PROMOTION_NONE);
    }
}

void chess_input_handle_board_mouse_motion(AppContext *ctx, int mouse_x, int mouse_y)
{
    if (!ctx || !ctx->ui.drag.drag_active) {
        return;
    }

    ctx->ui.drag.drag_mouse_x = mouse_x;
    ctx->ui.drag.drag_mouse_y = mouse_y;
}

void chess_input_handle_board_mouse_up(AppContext *ctx, int mouse_x, int mouse_y)
{
    int to_file;
    int to_rank;

    if (!ctx || !ctx->ui.drag.drag_active) {
        return;
    }

    ctx->ui.drag.drag_mouse_x = mouse_x;
    ctx->ui.drag.drag_mouse_y = mouse_y;

    if (ctx->ui.drag.promotion_pending) {
        ctx->ui.drag.drag_active = false;
        ctx->ui.drag.drag_piece = CHESS_PIECE_EMPTY;
        ctx->ui.drag.drag_from_file = -1;
        ctx->ui.drag.drag_from_rank = -1;
        return;
    }

    {
        bool move_ok = false;
        bool same_square = false;

        if (chess_ui_screen_to_board_square(ctx, mouse_x, mouse_y, &to_file, &to_rank)) {
            if (to_file == ctx->ui.drag.drag_from_file && to_rank == ctx->ui.drag.drag_from_rank) {
                /* Clicked (not dragged) on the same square */
                same_square = true;
                if (ctx->ui.drag.was_already_selected) {
                    /* Re-click on the selected piece → deselect */
                    chess_game_clear_selection(&ctx->game.game_state);
                }
                /* else: newly selected → keep selection for click-to-move */
            } else {
                move_ok = chess_input_try_send_local_move(ctx, to_file, to_rank, CHESS_PROMOTION_NONE);
            }
        }

        if (!move_ok && !same_square && !ctx->ui.drag.promotion_pending) {
            chess_ui_start_snap_back_animation(
                ctx,
                ctx->ui.drag.drag_piece,
                ctx->ui.drag.drag_from_file,
                ctx->ui.drag.drag_from_rank,
                (float)mouse_x,
                (float)mouse_y);
        }
    }

    ctx->ui.drag.drag_active = false;
    ctx->ui.drag.drag_piece = CHESS_PIECE_EMPTY;
    ctx->ui.drag.drag_from_file = -1;
    ctx->ui.drag.drag_from_rank = -1;
}
