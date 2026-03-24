#ifndef CHESS_APP_NETWORK_SESSION_H
#define CHESS_APP_NETWORK_SESSION_H

#include "chess_app/network_peer.h"
#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stdint.h>

/* ── Connection phase (replaces legacy ChessNetworkState + ad-hoc booleans) ── */

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
    /* Phase-based FSM */
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
    bool challenge_done;            /* OFFER/ACCEPT exchange completed          */
    bool resume_done;               /* resume negotiation finished (or skipped) */
    bool game_started;

    /* Handshake progress (moved from AppContext) */
    bool connect_attempted;
    bool hello_sent;
    bool hello_received;
    bool hello_completed;
    bool start_sent;
    uint64_t start_sent_at_ms;
    bool start_completed;
    bool resume_request_sent;
    bool pending_resume_state_sync;
    unsigned int start_failures;

    /* Draw offer state */
    bool draw_offer_pending;        /* we sent an offer, waiting for response   */
    bool draw_offer_received;       /* opponent sent an offer, we must respond  */
} ChessNetworkSession;

/* ── Session API ───────────────────────────────────────────────────────── */

void chess_network_session_init(ChessNetworkSession *session, const ChessPeerInfo *local_peer);
void chess_network_session_set_remote(ChessNetworkSession *session, const ChessPeerInfo *remote_peer);
void chess_network_session_start_game(ChessNetworkSession *session, uint32_t game_id, ChessPlayerColor local_color);

/* Phase transition helper (logs + updates phase_entered_at_ms) */
void chess_network_session_set_phase(ChessNetworkSession *session, ChessConnectionPhase new_phase);

#endif
