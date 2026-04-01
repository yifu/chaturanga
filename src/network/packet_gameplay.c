/**
 * Gameplay packet handlers: MOVE, RESIGN, DRAW_OFFER/ACCEPT/DECLINE.
 *
 * Split from packet_handlers.c for focused module size.
 */
#include "packet_handlers_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/game_state.h"
#include "chess_app/net_handler.h"
#include "chess_app/network_session.h"
#include "chess_app/notification.h"
#include "chess_app/persistence.h"
#include "chess_app/ui_game.h"

#include <SDL3/SDL.h>

/* ── MOVE ───────────────────────────────────────────────────────────── */

void chess_pkt_handle_move(AppContext *ctx, const ChessMovePayload *move)
{
    ChessPiece moving_piece;
    ChessPlayerColor remote_color;
    char notation[24];
    bool notation_ready = false;

    if (!ctx || !move || !ctx->network.network_session.game_started) {
        return;
    }

    remote_color = chess_pkt_opposite_color(ctx->network.network_session.local_color);
    if (remote_color == CHESS_COLOR_UNASSIGNED) {
        return;
    }

    moving_piece = chess_game_get_piece(&ctx->game.game_state, (int)move->from_file, (int)move->from_rank);

    if (chess_move_format_algebraic_notation(
            &ctx->game.game_state,
            (int)move->from_file,
            (int)move->from_rank,
            (int)move->to_file,
            (int)move->to_rank,
            move->promotion,
            notation,
            sizeof(notation))) {
        notation_ready = true;
    }

    /* Detect capture before the move modifies the board */
    {
        ChessPiece victim = chess_game_get_piece(
            &ctx->game.game_state, (int)move->to_file, (int)move->to_rank);
        int victim_file = (int)move->to_file;
        int victim_rank = (int)move->to_rank;

        if (victim == CHESS_PIECE_EMPTY) {
            /* En passant: pawn moving diagonally to empty square */
            ChessPiece mover = chess_game_get_piece(
                &ctx->game.game_state, (int)move->from_file, (int)move->from_rank);
            if ((mover == CHESS_PIECE_WHITE_PAWN || mover == CHESS_PIECE_BLACK_PAWN) &&
                move->to_file != move->from_file) {
                victim_rank = (int)move->from_rank;
                victim = chess_game_get_piece(
                    &ctx->game.game_state, (int)move->to_file, victim_rank);
            }
        }

        if (chess_game_apply_remote_move(&ctx->game.game_state, remote_color, move)) {
            /* Server: deduct time from the player who just moved and send TIME_SYNC */
            if (ctx->network.network_session.role == CHESS_ROLE_SERVER &&
                ctx->game.turn_started_at_ms > 0) {
                uint64_t now = SDL_GetTicks();
                uint64_t elapsed = now - ctx->game.turn_started_at_ms;
                uint32_t *mover_remaining = (remote_color == CHESS_COLOR_WHITE)
                    ? &ctx->game.white_remaining_ms
                    : &ctx->game.black_remaining_ms;
                if (elapsed >= *mover_remaining) {
                    *mover_remaining = 0;
                } else {
                    *mover_remaining -= (uint32_t)elapsed;
                }
                ctx->game.turn_started_at_ms = now;
                ctx->game.last_clock_sync_ticks = now;

                {
                    ChessTimeSyncPayload ts;
                    ts.white_remaining_ms = ctx->game.white_remaining_ms;
                    ts.black_remaining_ms = ctx->game.black_remaining_ms;
                    transport_send_time_sync(&ctx->network.transport.base, &ts);
                }
            }

            if (victim != CHESS_PIECE_EMPTY) {
                /* Defer capture animation until the remote-move slide finishes */
                ctx->ui.capture_anim.pending = true;
                ctx->ui.capture_anim.piece = victim;
                ctx->ui.capture_anim.from_file = victim_file;
                ctx->ui.capture_anim.from_rank = victim_rank;
            }

            ChessPiece piece_to_animate = moving_piece;
            if (piece_to_animate == CHESS_PIECE_EMPTY) {
                piece_to_animate = chess_game_get_piece(&ctx->game.game_state, (int)move->to_file, (int)move->to_rank);
            }

            if (piece_to_animate != CHESS_PIECE_EMPTY) {
                ctx->ui.remote_move_anim.active = true;
                ctx->ui.remote_move_anim.piece = piece_to_animate;
                ctx->ui.remote_move_anim.from_file = (int)move->from_file;
                ctx->ui.remote_move_anim.from_rank = (int)move->from_rank;
                ctx->ui.remote_move_anim.to_file = (int)move->to_file;
                ctx->ui.remote_move_anim.to_rank = (int)move->to_rank;
                ctx->ui.remote_move_anim.started_at_ms = SDL_GetTicks();
                if (ctx->ui.remote_move_anim.duration_ms == 0u) {
                    ctx->ui.remote_move_anim.duration_ms = CHESS_REMOTE_MOVE_ANIM_DEFAULT_MS;
                }
            }

            if (notation_ready) {
                app_append_move_history(ctx, notation);
            }

            if (ctx->network.network_session.role == CHESS_ROLE_SERVER) {
                (void)chess_persist_save_match_snapshot(ctx);
            }

            if (!ctx->win.window_has_focus && notation_ready) {
                char notif_body[48];
                SDL_snprintf(notif_body, sizeof(notif_body),
                             "Your opponent played %s", notation);
                chess_notification_send("Chaturanga", notif_body);
            }

            SDL_Log(
                "GAME: applied remote move (%u,%u) -> (%u,%u)",
                (unsigned)move->from_file,
                (unsigned)move->from_rank,
                (unsigned)move->to_file,
                (unsigned)move->to_rank
            );
        } else {
            SDL_Log("GAME: ignoring invalid remote MOVE payload");
        }
    }
}

/* ── RESIGN ─────────────────────────────────────────────────────────── */

void chess_pkt_handle_resign(AppContext *ctx)
{
    ChessPlayerColor remote_color;

    if (!ctx || !ctx->network.network_session.game_started ||
        ctx->game.game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    remote_color = chess_pkt_opposite_color(ctx->network.network_session.local_color);
    ctx->game.game_state.outcome = (remote_color == CHESS_COLOR_WHITE)
        ? CHESS_OUTCOME_WHITE_RESIGNED
        : CHESS_OUTCOME_BLACK_RESIGNED;

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: opponent resigned");
    app_set_status_message(ctx, "Opponent resigned.", 5000u);
}

/* ── DRAW_OFFER ─────────────────────────────────────────────────────── */

void chess_pkt_handle_draw_offer(AppContext *ctx)
{
    if (!ctx || !ctx->network.network_session.game_started ||
        ctx->game.game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    ctx->network.network_session.draw_offer_received = true;
    SDL_Log("GAME: opponent offers a draw");
    app_set_status_message(ctx, "Draw offered — Accept or Decline.", 30000u);
}

/* ── DRAW_ACCEPT ────────────────────────────────────────────────────── */

void chess_pkt_handle_draw_accept(AppContext *ctx)
{
    if (!ctx || !ctx->network.network_session.game_started ||
        ctx->game.game_state.outcome != CHESS_OUTCOME_NONE ||
        !ctx->network.network_session.draw_offer_pending) {
        return;
    }

    ctx->game.game_state.outcome = CHESS_OUTCOME_DRAW_AGREED;
    ctx->network.network_session.draw_offer_pending = false;

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: draw accepted");
    app_set_status_message(ctx, "Draw by agreement.", 5000u);
}

/* ── DRAW_DECLINE ───────────────────────────────────────────────────── */

void chess_pkt_handle_draw_decline(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->network.network_session.draw_offer_pending = false;
    SDL_Log("GAME: draw declined by opponent");
    app_set_status_message(ctx, "Draw declined.", 3000u);
}

/* ── TIME_SYNC ──────────────────────────────────────────────────────── */

void chess_pkt_handle_time_sync(AppContext *ctx, const ChessTimeSyncPayload *ts)
{
    if (!ctx || !ts || !ctx->network.network_session.game_started) {
        return;
    }

    ctx->game.white_remaining_ms = ts->white_remaining_ms;
    ctx->game.black_remaining_ms = ts->black_remaining_ms;
    ctx->game.last_clock_sync_ticks = SDL_GetTicks();

    /* Detect timeout signaled by server */
    if (ts->white_remaining_ms == 0 && ctx->game.game_state.outcome == CHESS_OUTCOME_NONE) {
        ctx->game.game_state.outcome = CHESS_OUTCOME_WHITE_TIMEOUT;
    } else if (ts->black_remaining_ms == 0 && ctx->game.game_state.outcome == CHESS_OUTCOME_NONE) {
        ctx->game.game_state.outcome = CHESS_OUTCOME_BLACK_TIMEOUT;
    }
}
