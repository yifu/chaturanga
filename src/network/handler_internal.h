/**
 * Internal header shared by the split handler_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/network/handler.c
 *   src/network/handler_challenges.c
 *   src/network/handler_connection.c
 */
#ifndef CHESS_APP_HANDLER_INTERNAL_H
#define CHESS_APP_HANDLER_INTERNAL_H

#include "chess_app/app_context.h"

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Socket polling result (shared between handler + challenges)        */
/* ------------------------------------------------------------------ */

#include "chess_app/network_discovery.h"
#include "chess_app/lobby_state.h"

typedef struct ChessNetPollResult {
    bool listener_readable;
    bool connection_readable;
    bool connection_writable;
    bool challenge_readable[CHESS_MAX_DISCOVERED_PEERS];
    bool challenge_writable[CHESS_MAX_DISCOVERED_PEERS];
    bool discovery_readable[CHESS_DISCOVERY_MAX_POLL_FDS];
    int  discovery_fd_count;
} ChessNetPollResult;

/* ------------------------------------------------------------------ */
/*  Challenge management (defined in handler_challenges.c)             */
/* ------------------------------------------------------------------ */

void chess_net_advance_outgoing_challenges(AppContext *ctx, const ChessNetPollResult *poll_result);

/* ------------------------------------------------------------------ */
/*  Connection establishment (defined in handler_connection.c)         */
/* ------------------------------------------------------------------ */

void chess_net_advance_transport_connection(AppContext *ctx, const ChessNetPollResult *socket_events);
void chess_net_advance_hello_handshake(AppContext *ctx);

#endif /* CHESS_APP_HANDLER_INTERNAL_H */
