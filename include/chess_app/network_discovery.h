#ifndef CHESS_APP_NETWORK_DISCOVERY_H
#define CHESS_APP_NETWORK_DISCOVERY_H

#include "chess_app/network_peer.h"

#include <stdbool.h>
#include <stdint.h>

#define CHESS_DISCOVERY_REMOVAL_QUEUE_MAX 16

typedef struct ChessDiscoveryContext {
    bool started;
    bool remote_emitted;
    uint16_t game_port;
    uint32_t local_ipv4;            /* LAN IP (host order), for loopback substitution */
    ChessPeerInfo local_peer;
    /* Queue of service-removal notifications for lobby */
    char removal_queue[CHESS_DISCOVERY_REMOVAL_QUEUE_MAX][CHESS_PROFILE_ID_STRING_LEN];
    int  removal_count;
    void *platform; /* platform-specific discovery backend data (heap-allocated) */
} ChessDiscoveryContext;

typedef struct ChessDiscoveredPeer {
    ChessPeerInfo peer;
    uint32_t tcp_ipv4;              /* remote IP (host order) for TCP connect   */
    uint16_t tcp_port;
} ChessDiscoveredPeer;

bool chess_discovery_start(ChessDiscoveryContext *ctx, ChessPeerInfo *local_peer, uint16_t game_port);
void chess_discovery_stop(ChessDiscoveryContext *ctx);
bool chess_discovery_poll(ChessDiscoveryContext *ctx, ChessDiscoveredPeer *out_remote_peer);
bool chess_discovery_poll_removal(ChessDiscoveryContext *ctx, char *out_profile_id, size_t out_size);

#endif
