#include "chess_app/network_session.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <string.h>

/* ── Phase-to-string helper ────────────────────────────────────────────── */

const char *chess_connection_phase_to_string(ChessConnectionPhase phase)
{
    switch (phase) {
    case CHESS_PHASE_IDLE:               return "IDLE";
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
        (strncmp(session->remote_peer.profile_id, remote_peer->profile_id, CHESS_PROFILE_ID_STRING_LEN) == 0);

    session->remote_peer = *remote_peer;
    session->peer_available = true;

    /* Do not regress the state machine when re-selecting the same peer from the lobby. */
    if (same_remote) {
        return;
    }

    session->transport_connected = false;
    session->hello_completed = false;
    session->challenge_done = false;
    session->resume_done = false;
    session->game_started = false;
    chess_network_session_set_phase(session, CHESS_PHASE_TCP_CONNECTING);
}

void chess_network_session_start_game(ChessNetworkSession *session, uint32_t game_id, ChessPlayerColor local_color)
{
    if (!session) {
        return;
    }

    session->game_id = game_id;
    session->local_color = local_color;
    session->game_started = true;
    session->draw_offer_pending = false;
    session->draw_offer_received = false;
}


