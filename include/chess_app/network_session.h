#ifndef CHESS_APP_NETWORK_SESSION_H
#define CHESS_APP_NETWORK_SESSION_H

#include "chess_app/network_peer.h"
#include "chess_app/network_protocol.h"

#include <stdbool.h>

typedef enum ChessNetworkState {
    CHESS_NET_IDLE_DISCOVERY = 0,
    CHESS_NET_PEER_FOUND,
    CHESS_NET_ELECTION,
    CHESS_NET_CONNECTING,
    CHESS_NET_IN_GAME,
    CHESS_NET_RECONNECTING,
    CHESS_NET_TERMINATED
} ChessNetworkState;

typedef struct ChessNetworkSession {
    ChessNetworkState state;
    ChessRole role;
    ChessPlayerColor local_color;
    ChessPeerInfo local_peer;
    ChessPeerInfo remote_peer;
    uint32_t game_id;
    bool peer_available;
    bool game_started;
    bool transport_ready;
} ChessNetworkSession;

void chess_network_session_init(ChessNetworkSession *session, const ChessPeerInfo *local_peer);
void chess_network_session_set_remote(ChessNetworkSession *session, const ChessPeerInfo *remote_peer);
void chess_network_session_set_transport_ready(ChessNetworkSession *session, bool transport_ready);
void chess_network_session_start_game(ChessNetworkSession *session, uint32_t game_id, ChessPlayerColor local_color);
void chess_network_session_step(ChessNetworkSession *session);

#endif
