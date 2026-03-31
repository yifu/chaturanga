#ifndef CHESS_APP_TRANSPORT_H
#define CHESS_APP_TRANSPORT_H

#include "chess_app/network_protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Receive result (transport-agnostic) ─────────────────────────────── */
typedef enum ChessRecvResult {
    CHESS_RECV_OK,
    CHESS_RECV_INCOMPLETE,
    CHESS_RECV_ERROR
} ChessRecvResult;

/* ── Transport vtable ────────────────────────────────────────────────── */
typedef struct TransportOps {
    bool (*send_packet)(void *self, uint32_t message_type, uint32_t sequence,
                        const void *payload, uint32_t payload_size);
    bool (*send_packet_pair)(void *self,
                             uint32_t msg_type_1, uint32_t seq_1, const void *payload_1, uint32_t payload_size_1,
                             uint32_t msg_type_2, uint32_t seq_2, const void *payload_2, uint32_t payload_size_2);
    ChessRecvResult (*recv_nonblocking)(void *self,
                                        ChessPacketHeader *out_header,
                                        uint8_t *out_payload, size_t payload_capacity);
    void (*recv_reset)(void *self);
    void (*close)(void *self);
    int  (*get_fd)(const void *self);
    bool (*set_nonblocking)(void *self);
} TransportOps;

/* ── Abstract transport handle ───────────────────────────────────────── */
typedef struct Transport {
    const TransportOps *ops;
} Transport;

/* ── Convenience inline wrappers ─────────────────────────────────────── */
static inline bool transport_send_packet(Transport *t, uint32_t msg_type, uint32_t seq,
                                          const void *payload, uint32_t payload_size) {
    return t && t->ops && t->ops->send_packet
        ? t->ops->send_packet(t, msg_type, seq, payload, payload_size)
        : false;
}

static inline bool transport_send_packet_pair(Transport *t,
                                               uint32_t msg_type_1, uint32_t seq_1, const void *payload_1, uint32_t payload_size_1,
                                               uint32_t msg_type_2, uint32_t seq_2, const void *payload_2, uint32_t payload_size_2) {
    return t && t->ops && t->ops->send_packet_pair
        ? t->ops->send_packet_pair(t, msg_type_1, seq_1, payload_1, payload_size_1,
                                   msg_type_2, seq_2, payload_2, payload_size_2)
        : false;
}

static inline ChessRecvResult transport_recv_nonblocking(Transport *t,
                                                          ChessPacketHeader *hdr,
                                                          uint8_t *payload, size_t cap) {
    return t && t->ops && t->ops->recv_nonblocking
        ? t->ops->recv_nonblocking(t, hdr, payload, cap)
        : CHESS_RECV_ERROR;
}

static inline void transport_recv_reset(Transport *t) {
    if (t && t->ops && t->ops->recv_reset) t->ops->recv_reset(t);
}

static inline void transport_close(Transport *t) {
    if (t && t->ops && t->ops->close) t->ops->close(t);
}

static inline int transport_get_fd(const Transport *t) {
    return t && t->ops && t->ops->get_fd ? t->ops->get_fd(t) : -1;
}

static inline bool transport_set_nonblocking(Transport *t) {
    return t && t->ops && t->ops->set_nonblocking ? t->ops->set_nonblocking(t) : false;
}

/* ── High-level convenience senders (use send_packet internally) ────── */
static inline bool transport_send_hello(Transport *t, const ChessHelloPayload *hello) {
    return transport_send_packet(t, CHESS_MSG_HELLO, 1u, hello, (uint32_t)sizeof(*hello));
}

static inline bool transport_send_ack(Transport *t, uint32_t acked_msg_type, uint32_t acked_seq, uint32_t status_code) {
    ChessAckPayload ack = { .acked_message_type = acked_msg_type, .acked_sequence = acked_seq, .status_code = status_code };
    return transport_send_packet(t, CHESS_MSG_ACK, acked_seq, &ack, (uint32_t)sizeof(ack));
}

static inline bool transport_send_start(Transport *t, const ChessStartPayload *start) {
    return transport_send_packet(t, CHESS_MSG_START, 2u, start, (uint32_t)sizeof(*start));
}

static inline bool transport_send_offer(Transport *t, const ChessOfferPayload *offer) {
    return transport_send_packet(t, CHESS_MSG_OFFER, 1u, offer, (uint32_t)sizeof(*offer));
}

static inline bool transport_send_accept(Transport *t, const ChessAcceptPayload *accept) {
    return transport_send_packet(t, CHESS_MSG_ACCEPT, 1u, accept, (uint32_t)sizeof(*accept));
}

static inline bool transport_send_resume_request(Transport *t, const ChessResumeRequestPayload *req) {
    return transport_send_packet(t, CHESS_MSG_RESUME_REQUEST, 3u, req, (uint32_t)sizeof(*req));
}

static inline bool transport_send_resume_response(Transport *t, const ChessResumeResponsePayload *resp) {
    return transport_send_packet(t, CHESS_MSG_RESUME_RESPONSE, 3u, resp, (uint32_t)sizeof(*resp));
}

static inline bool transport_send_state_snapshot(Transport *t, const ChessStateSnapshotPayload *snap) {
    return transport_send_packet(t, CHESS_MSG_STATE_SNAPSHOT, 4u, snap, (uint32_t)sizeof(*snap));
}

static inline bool transport_send_time_sync(Transport *t, const ChessTimeSyncPayload *ts) {
    return transport_send_packet(t, CHESS_MSG_TIME_SYNC, 0u, ts, (uint32_t)sizeof(*ts));
}

static inline bool transport_send_move_with_time_sync(
    Transport *t, uint32_t move_seq, const ChessMovePayload *move, const ChessTimeSyncPayload *ts) {
    return transport_send_packet_pair(t,
        CHESS_MSG_MOVE, move_seq, move, (uint32_t)sizeof(*move),
        CHESS_MSG_TIME_SYNC, 0u, ts, (uint32_t)sizeof(*ts));
}

#endif /* CHESS_APP_TRANSPORT_H */
