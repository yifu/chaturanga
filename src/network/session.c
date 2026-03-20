#include "chess_app/network_session.h"

#include <stddef.h>
#include <string.h>

void chess_network_session_init(ChessNetworkSession *session, const ChessPeerInfo *local_peer)
{
    if (!session || !local_peer) {
        return;
    }

    memset(session, 0, sizeof(*session));
    session->state = CHESS_NET_IDLE_DISCOVERY;
    session->role = CHESS_ROLE_UNKNOWN;
    session->local_peer = *local_peer;
}

void chess_network_session_set_remote(ChessNetworkSession *session, const ChessPeerInfo *remote_peer)
{
    if (!session || !remote_peer) {
        return;
    }

    session->remote_peer = *remote_peer;
    session->peer_available = true;
    session->transport_ready = false;
    session->state = CHESS_NET_PEER_FOUND;
}

void chess_network_session_set_transport_ready(ChessNetworkSession *session, bool transport_ready)
{
    if (!session) {
        return;
    }

    session->transport_ready = transport_ready;
}

void chess_network_session_step(ChessNetworkSession *session)
{
    if (!session) {
        return;
    }

    switch (session->state) {
    case CHESS_NET_IDLE_DISCOVERY:
        break;
    case CHESS_NET_PEER_FOUND:
        session->state = CHESS_NET_ELECTION;
        break;
    case CHESS_NET_ELECTION:
        session->role = chess_elect_role(&session->local_peer, &session->remote_peer);
        session->state = CHESS_NET_CONNECTING;
        break;
    case CHESS_NET_CONNECTING:
        if (session->transport_ready) {
            session->state = CHESS_NET_IN_GAME;
        }
        break;
    case CHESS_NET_IN_GAME:
        break;
    case CHESS_NET_RECONNECTING:
        session->state = CHESS_NET_CONNECTING;
        break;
    case CHESS_NET_TERMINATED:
        break;
    default:
        session->state = CHESS_NET_TERMINATED;
        break;
    }
}
