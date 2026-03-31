#ifndef CHESS_APP_NETWORK_CONTEXT_H
#define CHESS_APP_NETWORK_CONTEXT_H

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/tcp_transport.h"

#include <stdint.h>

typedef struct NetworkContext {
    ChessPeerInfo local_peer;
    ChessNetworkSession network_session;
    ChessDiscoveryContext discovery;
    ChessTcpListener listener;
    TcpTransport transport;
    ChessDiscoveredPeer discovered_peer;
    int connect_retry_ms;
    uint64_t next_connect_attempt_at;
} NetworkContext;

#endif
