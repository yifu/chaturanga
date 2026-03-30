#ifndef CHESS_APP_TCP_TRANSPORT_H
#define CHESS_APP_TCP_TRANSPORT_H

#include "chess_app/transport.h"
#include "chess_app/network_tcp.h"

/* ── TCP transport (concrete implementation) ─────────────────────────── */
typedef struct TcpTransport {
    Transport base;                 /* MUST be first member (allows casting) */
    ChessTcpConnection connection;
    ChessTcpRecvBuffer recv_buffer;
} TcpTransport;

/* Initialize a TcpTransport (sets the vtable pointer, resets state) */
void tcp_transport_init(TcpTransport *tcp);

/* Initialize from an already-connected fd */
void tcp_transport_init_from_fd(TcpTransport *tcp, int fd);

/* Access the static vtable (for external use if needed) */
extern const TransportOps tcp_transport_ops;

#endif /* CHESS_APP_TCP_TRANSPORT_H */
