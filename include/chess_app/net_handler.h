#ifndef CHESS_APP_NET_HANDLER_H
#define CHESS_APP_NET_HANDLER_H

#include "chess_app/app_context.h"

#define CHESS_REMOTE_MOVE_ANIM_DEFAULT_MS 160u

/**
 * Reset all transport / handshake progress flags on the context.
 * Called after disconnect and during init.
 */
void chess_net_reset_transport_progress(AppContext *ctx);

/**
 * Run one network tick: advance transport connection, handshake,
 * drain incoming packets, send pending start/resume/snapshot.
 */
void chess_net_tick(AppContext *ctx);

#endif
