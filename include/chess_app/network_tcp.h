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

bool chess_tcp_send_hello(ChessTcpConnection *conn, const ChessHelloPayload *hello);
bool chess_tcp_recv_hello(ChessTcpConnection *conn, int timeout_ms, ChessHelloPayload *out_hello);
bool chess_tcp_send_ack(ChessTcpConnection *conn);
bool chess_tcp_recv_ack(ChessTcpConnection *conn, int timeout_ms);

#endif
