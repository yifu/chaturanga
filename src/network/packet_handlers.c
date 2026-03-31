/**
 * Packet dispatch and receive — routes incoming packets to the
 * specialised handlers in packet_handshake.c, packet_game_init.c,
 * and packet_gameplay.c.
 */
#include "packet_handlers_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/net_handler.h"
#include "chess_app/network_protocol.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Packet receive ─────────────────────────────────────────────────── */

static ChessRecvResult net_receive_next_packet(AppContext *ctx, ChessPacketHeader *header, uint8_t *payload, size_t payload_capacity)
{
    ChessRecvResult result;
    Transport *t;

    if (!ctx || !header || !payload) {
        return CHESS_RECV_ERROR;
    }

    t = &ctx->network.transport.base;
    result = transport_recv_nonblocking(t, header, payload, payload_capacity);

    if (result == CHESS_RECV_ERROR) {
        SDL_Log("NET: recv error on game connection, closing");
        app_handle_peer_disconnect(ctx, "recv error on game connection");
    }

    return result;
}

/* ── Packet dispatch ────────────────────────────────────────────────── */

static void net_dispatch_incoming_packet(AppContext *ctx, const ChessPacketHeader *header, const uint8_t *payload)
{
    if (!ctx || !header || !payload) {
        return;
    }

    if (header->message_type == CHESS_MSG_HELLO && header->payload_size == sizeof(ChessHelloPayload)) {
        chess_pkt_handle_hello(ctx, (const ChessHelloPayload *)payload);
    } else if (header->message_type == CHESS_MSG_OFFER && header->payload_size == sizeof(ChessOfferPayload)) {
        chess_pkt_handle_offer(ctx, (const ChessOfferPayload *)payload);
    } else if (header->message_type == CHESS_MSG_ACCEPT && header->payload_size == sizeof(ChessAcceptPayload)) {
        chess_pkt_handle_accept(ctx, (const ChessAcceptPayload *)payload);
    } else if (header->message_type == CHESS_MSG_START && header->payload_size == sizeof(ChessStartPayload)) {
        chess_pkt_handle_start(ctx, (const ChessStartPayload *)payload);
    } else if (header->message_type == CHESS_MSG_ACK && header->payload_size == sizeof(ChessAckPayload)) {
        chess_pkt_handle_ack(ctx, (const ChessAckPayload *)payload);
    } else if (header->message_type == CHESS_MSG_MOVE && header->payload_size == sizeof(ChessMovePayload)) {
        chess_pkt_handle_move(ctx, (const ChessMovePayload *)payload);
    } else if (header->message_type == CHESS_MSG_RESUME_REQUEST &&
               header->payload_size == sizeof(ChessResumeRequestPayload)) {
        chess_pkt_handle_resume_request(ctx, (const ChessResumeRequestPayload *)payload);
    } else if (header->message_type == CHESS_MSG_RESUME_RESPONSE &&
               header->payload_size == sizeof(ChessResumeResponsePayload)) {
        chess_pkt_handle_resume_response(ctx, (const ChessResumeResponsePayload *)payload);
    } else if (header->message_type == CHESS_MSG_STATE_SNAPSHOT &&
               header->payload_size == sizeof(ChessStateSnapshotPayload)) {
        chess_pkt_handle_state_snapshot(ctx, (const ChessStateSnapshotPayload *)payload);
    } else if (header->message_type == CHESS_MSG_RESIGN && header->payload_size == 0u) {
        chess_pkt_handle_resign(ctx);
    } else if (header->message_type == CHESS_MSG_DRAW_OFFER && header->payload_size == 0u) {
        chess_pkt_handle_draw_offer(ctx);
    } else if (header->message_type == CHESS_MSG_DRAW_ACCEPT && header->payload_size == 0u) {
        chess_pkt_handle_draw_accept(ctx);
    } else if (header->message_type == CHESS_MSG_DRAW_DECLINE && header->payload_size == 0u) {
        chess_pkt_handle_draw_decline(ctx);
    } else if (header->message_type == CHESS_MSG_TIME_SYNC &&
               header->payload_size == sizeof(ChessTimeSyncPayload)) {
        chess_pkt_handle_time_sync(ctx, (const ChessTimeSyncPayload *)payload);
    }
}

/* ── Public entry point ─────────────────────────────────────────────── */

void chess_net_drain_incoming_packets(AppContext *ctx, bool initially_readable)
{
    const int max_packets_per_frame = 8;
    int packet_idx;

    if (!ctx || transport_get_fd(&ctx->network.transport.base) < 0 || !initially_readable) {
        return;
    }

    for (packet_idx = 0; packet_idx < max_packets_per_frame; ++packet_idx) {
        ChessPacketHeader header;
        uint8_t payload[sizeof(ChessStateSnapshotPayload)];
        ChessRecvResult result = net_receive_next_packet(ctx, &header, payload, sizeof(payload));

        if (result != CHESS_RECV_OK) {
            break;
        }

        net_dispatch_incoming_packet(ctx, &header, payload);
    }
}
