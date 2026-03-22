#ifndef CHESS_APP_NETWORK_TCP_H
#define CHESS_APP_NETWORK_TCP_H

#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ChessTcpListener {
    int fd;
    uint16_t port;
} ChessTcpListener;

typedef struct ChessTcpConnection {
    int fd;
} ChessTcpConnection;

bool chess_tcp_listener_open(ChessTcpListener *listener, uint16_t requested_port);
void chess_tcp_listener_close(ChessTcpListener *listener);

bool chess_tcp_accept_once(ChessTcpListener *listener, int timeout_ms, ChessTcpConnection *out_conn);
bool chess_tcp_connect_once(uint32_t remote_ipv4_host_order, uint16_t remote_port, int timeout_ms, ChessTcpConnection *out_conn);
void chess_tcp_connection_close(ChessTcpConnection *conn);

bool chess_tcp_send_packet(ChessTcpConnection *conn, uint32_t message_type, uint32_t sequence, const void *payload, uint32_t payload_size);
bool chess_tcp_recv_packet_header(ChessTcpConnection *conn, int timeout_ms, ChessPacketHeader *out_header);
bool chess_tcp_recv_payload(ChessTcpConnection *conn, int timeout_ms, void *out_payload, uint32_t payload_size);

bool chess_tcp_send_hello(ChessTcpConnection *conn, const ChessHelloPayload *hello);
bool chess_tcp_recv_hello(ChessTcpConnection *conn, int timeout_ms, ChessHelloPayload *out_hello);
bool chess_tcp_send_ack(ChessTcpConnection *conn, uint32_t acked_message_type, uint32_t acked_sequence, uint32_t status_code);
bool chess_tcp_recv_ack(ChessTcpConnection *conn, int timeout_ms, ChessAckPayload *out_ack);
bool chess_tcp_send_start(ChessTcpConnection *conn, const ChessStartPayload *start);
bool chess_tcp_recv_start(ChessTcpConnection *conn, int timeout_ms, ChessStartPayload *out_start);
bool chess_tcp_send_offer(ChessTcpConnection *conn, const ChessOfferPayload *offer);
bool chess_tcp_recv_offer(ChessTcpConnection *conn, int timeout_ms, ChessOfferPayload *out_offer);
bool chess_tcp_send_accept(ChessTcpConnection *conn, const ChessAcceptPayload *accept);
bool chess_tcp_recv_accept(ChessTcpConnection *conn, int timeout_ms, ChessAcceptPayload *out_accept);

#endif
