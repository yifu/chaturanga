#include "chess_app/network_session.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <string.h>

/* ── Phase-to-string helper ────────────────────────────────────────────── */

const char *chess_connection_phase_to_string(ChessConnectionPhase phase)
{
    switch (phase) {
    case CHESS_PHASE_IDLE:               return "IDLE";
    case CHESS_PHASE_ELECTING:           return "ELECTING";
    case CHESS_PHASE_TCP_CONNECTING:     return "TCP_CONNECTING";
    case CHESS_PHASE_HELLO_HANDSHAKE:    return "HELLO_HANDSHAKE";
    case CHESS_PHASE_AUTHENTICATED:      return "AUTHENTICATED";
    case CHESS_PHASE_RESUME_NEGOTIATING: return "RESUME_NEGOTIATING";
    case CHESS_PHASE_GAME_STARTING:      return "GAME_STARTING";
    case CHESS_PHASE_IN_GAME:            return "IN_GAME";
    case CHESS_PHASE_DISCONNECTED:       return "DISCONNECTED";
    case CHESS_PHASE_TERMINATED:         return "TERMINATED";
    default:                             return "UNKNOWN";
    }
}

/* ── Phase transition ──────────────────────────────────────────────────── */

void chess_network_session_set_phase(ChessNetworkSession *session, ChessConnectionPhase new_phase)
{
    if (!session || session->phase == new_phase) {
        return;
    }

    SDL_Log("NET: phase %s -> %s",
            chess_connection_phase_to_string(session->phase),
            chess_connection_phase_to_string(new_phase));
    session->phase = new_phase;
    session->phase_entered_at_ms = SDL_GetTicks();
}

void chess_network_session_init(ChessNetworkSession *session, const ChessPeerInfo *local_peer)
{
    if (!session || !local_peer) {
        return;
    }

    memset(session, 0, sizeof(*session));
    session->state = CHESS_NET_IDLE_DISCOVERY;
    session->phase = CHESS_PHASE_IDLE;
    session->phase_entered_at_ms = SDL_GetTicks();
    session->role = CHESS_ROLE_UNKNOWN;
    session->local_color = CHESS_COLOR_UNASSIGNED;
    session->local_peer = *local_peer;
}

void chess_network_session_set_remote(ChessNetworkSession *session, const ChessPeerInfo *remote_peer)
{
    bool same_remote;

    if (!session || !remote_peer) {
        return;
    }

    same_remote =
        session->peer_available &&
        (strncmp(session->remote_peer.uuid, remote_peer->uuid, CHESS_UUID_STRING_LEN) == 0);

    session->remote_peer = *remote_peer;
    session->peer_available = true;

    /* Do not regress the state machine when re-selecting the same peer from the lobby. */
    if (same_remote) {
        return;
    }

    session->transport_ready = false;
    session->transport_connected = false;
    session->hello_done = false;
    session->challenge_done = false;
    session->resume_done = false;
    session->game_started = false;
    session->state = CHESS_NET_PEER_FOUND;
    chess_network_session_set_phase(session, CHESS_PHASE_ELECTING);
}

void chess_network_session_set_transport_ready(ChessNetworkSession *session, bool transport_ready)
{
    if (!session) {
        return;
    }

    session->transport_ready = transport_ready;
}

void chess_network_session_start_game(ChessNetworkSession *session, uint32_t game_id, ChessPlayerColor local_color)
{
    if (!session) {
        return;
    }

    session->game_id = game_id;
    session->local_color = local_color;
    session->game_started = true;
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

/* ── New phase-based step ──────────────────────────────────────────────── */

void chess_network_session_step_phase(ChessNetworkSession *session)
{
    if (!session) {
        return;
    }

    switch (session->phase) {
    case CHESS_PHASE_IDLE:
        /* Wait for peer discovery (set_remote triggers ELECTING). */
        break;

    case CHESS_PHASE_ELECTING:
        session->role = chess_elect_role(&session->local_peer, &session->remote_peer);
        chess_network_session_set_phase(session, CHESS_PHASE_TCP_CONNECTING);
        break;

    case CHESS_PHASE_TCP_CONNECTING:
        if (session->transport_connected) {
            chess_network_session_set_phase(session, CHESS_PHASE_HELLO_HANDSHAKE);
        }
        break;

    case CHESS_PHASE_HELLO_HANDSHAKE:
        if (session->hello_done) {
            chess_network_session_set_phase(session, CHESS_PHASE_AUTHENTICATED);
        }
        break;

    case CHESS_PHASE_AUTHENTICATED:
        /* Lobby challenge flow happens here (OFFER/ACCEPT).
         * Transition to RESUME_NEGOTIATING or GAME_STARTING is driven
         * by the app layer once challenge_done is set. */
        break;

    case CHESS_PHASE_RESUME_NEGOTIATING:
        if (session->resume_done) {
            chess_network_session_set_phase(session, CHESS_PHASE_GAME_STARTING);
        }
        break;

    case CHESS_PHASE_GAME_STARTING:
        if (session->game_started) {
            chess_network_session_set_phase(session, CHESS_PHASE_IN_GAME);
        }
        break;

    case CHESS_PHASE_IN_GAME:
        /* Gameplay active — no automatic transition. */
        break;

    case CHESS_PHASE_DISCONNECTED:
        /* App layer decides whether to re-elect or terminate. */
        break;

    case CHESS_PHASE_TERMINATED:
        break;

    default:
        chess_network_session_set_phase(session, CHESS_PHASE_TERMINATED);
        break;
    }
}
