#ifndef CHESS_APP_NETWORK_TCP_H
#define CHESS_APP_NETWORK_TCP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ChessTcpListener {
    int fd;
    uint16_t port;
} ChessTcpListener;

bool chess_tcp_listener_open(ChessTcpListener *listener, uint16_t requested_port);
void chess_tcp_listener_close(ChessTcpListener *listener);

#endif
