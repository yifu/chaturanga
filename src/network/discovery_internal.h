/**
 * Internal header shared by the split discovery_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/network/discovery.c
 *   src/network/discovery_dnssd.c
 *   src/network/discovery_avahi.c
 */
#ifndef CHESS_APP_DISCOVERY_INTERNAL_H
#define CHESS_APP_DISCOVERY_INTERNAL_H

#include "chess_app/network_discovery.h"

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  DNS-SD (Bonjour) backend — macOS                                   */
/* ------------------------------------------------------------------ */

#if defined(CHESS_APP_HAVE_DNSSD)

bool chess_discovery_dnssd_start(ChessDiscoveryContext *ctx);
void chess_discovery_dnssd_stop(ChessDiscoveryContext *ctx);
int  chess_discovery_dnssd_get_poll_fds(ChessDiscoveryContext *ctx,
                                        int *out_fds, int max_fds);
void chess_discovery_dnssd_process_events(ChessDiscoveryContext *ctx,
                                          const bool *readable, int fd_count);
bool chess_discovery_dnssd_check_result(ChessDiscoveryContext *ctx,
                                        ChessDiscoveredPeer *out);

#endif /* CHESS_APP_HAVE_DNSSD */

/* ------------------------------------------------------------------ */
/*  Avahi backend — Linux                                              */
/* ------------------------------------------------------------------ */

#if defined(CHESS_APP_HAVE_AVAHI)

bool chess_discovery_avahi_start(ChessDiscoveryContext *ctx);
void chess_discovery_avahi_stop(ChessDiscoveryContext *ctx);
int  chess_discovery_avahi_get_poll_fds(ChessDiscoveryContext *ctx,
                                        int *out_fds, int max_fds);
void chess_discovery_avahi_process_events(ChessDiscoveryContext *ctx,
                                          const bool *readable, int fd_count);
bool chess_discovery_avahi_check_result(ChessDiscoveryContext *ctx,
                                        ChessDiscoveredPeer *out);

#endif /* CHESS_APP_HAVE_AVAHI */

#endif /* CHESS_APP_DISCOVERY_INTERNAL_H */
