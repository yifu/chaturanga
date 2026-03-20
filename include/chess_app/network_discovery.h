#ifndef CHESS_APP_NETWORK_DISCOVERY_H
#define CHESS_APP_NETWORK_DISCOVERY_H

#include "chess_app/network_peer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ChessDiscoveryContext {
    bool started;
    bool remote_emitted;
    uint16_t game_port;
    ChessPeerInfo local_peer;
} ChessDiscoveryContext;

typedef struct ChessDiscoveredPeer {
    ChessPeerInfo peer;
    uint16_t tcp_port;
} ChessDiscoveredPeer;

bool chess_discovery_start(ChessDiscoveryContext *ctx, const ChessPeerInfo *local_peer, uint16_t game_port);
void chess_discovery_stop(ChessDiscoveryContext *ctx);
bool chess_discovery_poll(ChessDiscoveryContext *ctx, ChessDiscoveredPeer *out_remote_peer);

#endif
