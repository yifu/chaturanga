#ifndef CHESS_APP_NETWORK_TCP_H
#define CHESS_APP_NETWORK_TCP_H

#include "chess_app/network_protocol.h"
#include "chess_app/transport.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ChessTcpListener {
    int fd;
    uint16_t port;
} ChessTcpListener;

typedef struct ChessTcpConnection {
    int fd;
} ChessTcpConnection;

/* ── Non-blocking receive automaton ─────────────────────────────────── */

typedef enum ChessRecvState {
    CHESS_RECV_WAITING_HEADER = 0,
    CHESS_RECV_WAITING_PAYLOAD
} ChessRecvState;

typedef struct ChessTcpRecvBuffer {
    ChessRecvState state;
    size_t done;
    uint8_t buf[sizeof(ChessPacketHeader) + sizeof(ChessStateSnapshotPayload)];
} ChessTcpRecvBuffer;

bool chess_tcp_listener_open(ChessTcpListener *listener, uint16_t requested_port);
void chess_tcp_listener_close(ChessTcpListener *listener);

bool chess_tcp_accept_once(ChessTcpListener *listener, int timeout_ms, ChessTcpConnection *out_conn);
void chess_tcp_connection_close(ChessTcpConnection *conn);

typedef enum ChessConnectResult {
    CHESS_CONNECT_CONNECTED,
    CHESS_CONNECT_IN_PROGRESS,
    CHESS_CONNECT_FAILED
} ChessConnectResult;

bool chess_tcp_connect_start(uint32_t remote_ipv4_host_order, uint16_t remote_port, int *out_fd);
ChessConnectResult chess_tcp_connect_check(int fd);

bool chess_tcp_send_packet(ChessTcpConnection *conn, uint32_t message_type, uint32_t sequence, const void *payload, uint32_t payload_size);
bool chess_tcp_send_packet_pair(
    ChessTcpConnection *conn,
    uint32_t msg_type_1, uint32_t seq_1, const void *payload_1, uint32_t payload_size_1,
    uint32_t msg_type_2, uint32_t seq_2, const void *payload_2, uint32_t payload_size_2);
bool chess_tcp_recv_packet_header(ChessTcpConnection *conn, int timeout_ms, ChessPacketHeader *out_header);
bool chess_tcp_recv_payload(ChessTcpConnection *conn, int timeout_ms, void *out_payload, uint32_t payload_size);

bool chess_tcp_set_nonblocking(ChessTcpConnection *conn);
void chess_tcp_recv_reset(ChessTcpRecvBuffer *rb);
ChessRecvResult chess_tcp_recv_nonblocking(ChessTcpConnection *conn, ChessTcpRecvBuffer *rb,
                                           ChessPacketHeader *out_header,
                                           uint8_t *out_payload, size_t payload_capacity);

bool chess_tcp_send_hello(ChessTcpConnection *conn, const ChessHelloPayload *hello);
bool chess_tcp_recv_hello(ChessTcpConnection *conn, int timeout_ms, ChessHelloPayload *out_hello);
bool chess_tcp_send_ack(ChessTcpConnection *conn, uint32_t acked_message_type, uint32_t acked_sequence, uint32_t status_code);
bool chess_tcp_recv_ack(ChessTcpConnection *conn, int timeout_ms, ChessAckPayload *out_ack);
bool chess_tcp_send_start(ChessTcpConnection *conn, const ChessStartPayload *start);
bool chess_tcp_send_offer(ChessTcpConnection *conn, const ChessOfferPayload *offer);
bool chess_tcp_send_accept(ChessTcpConnection *conn, const ChessAcceptPayload *accept);
bool chess_tcp_send_resume_request(ChessTcpConnection *conn, const ChessResumeRequestPayload *request);
bool chess_tcp_send_resume_response(ChessTcpConnection *conn, const ChessResumeResponsePayload *response);
bool chess_tcp_send_state_snapshot(ChessTcpConnection *conn, const ChessStateSnapshotPayload *snapshot);

#endif
