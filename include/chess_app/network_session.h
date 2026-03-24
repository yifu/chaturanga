#ifndef CHESS_APP_NETWORK_SESSION_H
#define CHESS_APP_NETWORK_SESSION_H

#include "chess_app/network_peer.h"
#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stdint.h>

/* ── Legacy state enum (kept during migration, will be removed) ────────── */

typedef enum ChessNetworkState {
    CHESS_NET_IDLE_DISCOVERY = 0,
    CHESS_NET_PEER_FOUND,
    CHESS_NET_ELECTION,
    CHESS_NET_CONNECTING,
    CHESS_NET_IN_GAME,
    CHESS_NET_RECONNECTING,
    CHESS_NET_TERMINATED
} ChessNetworkState;

/* ── New explicit connection phase (replaces state + 11 ad-hoc booleans) ─ */

typedef enum ChessConnectionPhase {
    CHESS_PHASE_IDLE = 0,           /* listening + browsing, lobby visible      */
    CHESS_PHASE_TCP_CONNECTING,     /* TCP connect (client) or accept (server)  */
    CHESS_PHASE_HELLO_HANDSHAKE,    /* HELLO exchange                           */
    CHESS_PHASE_AUTHENTICATED,      /* transport ready, challenge lobby open    */
    CHESS_PHASE_RESUME_NEGOTIATING, /* RESUME_REQUEST / RESPONSE (optional)     */
    CHESS_PHASE_GAME_STARTING,      /* START / ACK + snapshot sync              */
    CHESS_PHASE_IN_GAME,            /* gameplay active                          */
    CHESS_PHASE_DISCONNECTED,       /* peer lost, reconnection possible         */
    CHESS_PHASE_TERMINATED          /* fatal error                              */
} ChessConnectionPhase;

const char *chess_connection_phase_to_string(ChessConnectionPhase phase);

/* ── Session structure ─────────────────────────────────────────────────── */

typedef struct ChessNetworkSession {
    /* Legacy (kept during migration) */
    ChessNetworkState state;

    /* New phase-based FSM */
    ChessConnectionPhase phase;
    uint64_t phase_entered_at_ms;   /* SDL_GetTicks() when phase was entered   */

    /* Identity & role */
    ChessRole role;
    ChessPlayerColor local_color;
    ChessPeerInfo local_peer;
    ChessPeerInfo remote_peer;

    /* Game tracking */
    uint32_t game_id;

    /* Flags consumed by the phase machine */
    bool peer_available;
    bool transport_connected;       /* TCP fd is valid                          */
    bool hello_done;                /* HELLO exchange completed                 */
    bool challenge_done;            /* OFFER/ACCEPT exchange completed          */
    bool resume_done;               /* resume negotiation finished (or skipped) */
    bool game_started;

    /* Legacy flag (kept during migration) */
    bool transport_ready;
} ChessNetworkSession;

/* ── Session API ───────────────────────────────────────────────────────── */

void chess_network_session_init(ChessNetworkSession *session, const ChessPeerInfo *local_peer);
void chess_network_session_set_remote(ChessNetworkSession *session, const ChessPeerInfo *remote_peer);
void chess_network_session_set_transport_ready(ChessNetworkSession *session, bool transport_ready);
void chess_network_session_start_game(ChessNetworkSession *session, uint32_t game_id, ChessPlayerColor local_color);

/* Legacy step — drives old ChessNetworkState transitions */
void chess_network_session_step(ChessNetworkSession *session);

/* New phase step — drives ChessConnectionPhase transitions */
void chess_network_session_step_phase(ChessNetworkSession *session);

/* Phase transition helper (logs + updates phase_entered_at_ms) */
void chess_network_session_set_phase(ChessNetworkSession *session, ChessConnectionPhase new_phase);

#endif
