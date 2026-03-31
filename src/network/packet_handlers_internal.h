/**
 * Internal header shared by the split packet_handlers_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/network/packet_handlers.c
 *   src/network/packet_handshake.c
 *   src/network/packet_game_init.c
 *   src/network/packet_gameplay.c
 */
#ifndef CHESS_APP_PACKET_HANDLERS_INTERNAL_H
#define CHESS_APP_PACKET_HANDLERS_INTERNAL_H

#include "chess_app/app_context.h"
#include "chess_app/network_protocol.h"

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

static inline ChessPlayerColor chess_pkt_opposite_color(ChessPlayerColor color)
{
    if (color == CHESS_COLOR_WHITE) {
        return CHESS_COLOR_BLACK;
    }
    if (color == CHESS_COLOR_BLACK) {
        return CHESS_COLOR_WHITE;
    }
    return CHESS_COLOR_UNASSIGNED;
}

/* ------------------------------------------------------------------ */
/*  Handshake phase (defined in packet_handshake.c)                    */
/* ------------------------------------------------------------------ */

void chess_pkt_handle_hello(AppContext *ctx, const ChessHelloPayload *hello);
void chess_pkt_handle_offer(AppContext *ctx, const ChessOfferPayload *offer);
void chess_pkt_handle_accept(AppContext *ctx, const ChessAcceptPayload *accept);

/* ------------------------------------------------------------------ */
/*  Game initialization (defined in packet_game_init.c)                */
/* ------------------------------------------------------------------ */

void chess_pkt_handle_start(AppContext *ctx, const ChessStartPayload *start_payload);
void chess_pkt_handle_ack(AppContext *ctx, const ChessAckPayload *ack);
void chess_pkt_handle_resume_request(AppContext *ctx, const ChessResumeRequestPayload *request);
void chess_pkt_handle_resume_response(AppContext *ctx, const ChessResumeResponsePayload *response);
void chess_pkt_handle_state_snapshot(AppContext *ctx, const ChessStateSnapshotPayload *snapshot);

/* ------------------------------------------------------------------ */
/*  Gameplay (defined in packet_gameplay.c)                            */
/* ------------------------------------------------------------------ */

void chess_pkt_handle_move(AppContext *ctx, const ChessMovePayload *move);
void chess_pkt_handle_resign(AppContext *ctx);
void chess_pkt_handle_draw_offer(AppContext *ctx);
void chess_pkt_handle_draw_accept(AppContext *ctx);
void chess_pkt_handle_draw_decline(AppContext *ctx);
void chess_pkt_handle_time_sync(AppContext *ctx, const ChessTimeSyncPayload *ts);

#endif /* CHESS_APP_PACKET_HANDLERS_INTERNAL_H */
