/* ============================================================
 * Public API for mDNS/DNS-SD peer discovery.
 *
 * Platform-specific backends live in dedicated translation units:
 *   discovery_dnssd.c  — DNS-SD / Bonjour (macOS)
 *   discovery_avahi.c  — Avahi (Linux)
 *
 * When neither backend is available the fallback reads peer info
 * from environment variables (CHESS_REMOTE_IP, CHESS_REMOTE_PORT,
 * CHESS_REMOTE_PROFILE_ID).
 * ============================================================ */

#include "discovery_internal.h"

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>


/* ============================================================
 * Public API
 * ============================================================ */

bool chess_discovery_start(ChessDiscoveryContext *ctx, ChessPeerInfo *local_peer, uint16_t game_port)
{
    if (!ctx || !local_peer) {
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->started    = true;
    ctx->game_port  = game_port;
    ctx->local_peer = *local_peer;

#if defined(CHESS_APP_HAVE_AVAHI)
    return chess_discovery_avahi_start(ctx);
#elif defined(CHESS_APP_HAVE_DNSSD)
    return chess_discovery_dnssd_start(ctx);
#else
    SDL_Log(
        "mDNS discovery backend unavailable, using env simulation. Local TCP port is %u."
        " Set CHESS_REMOTE_IP, CHESS_REMOTE_PORT and CHESS_REMOTE_UUID.",
        (unsigned int)ctx->game_port);
    return true;
#endif
}

void chess_discovery_stop(ChessDiscoveryContext *ctx)
{
    if (!ctx) {
        return;
    }

#if defined(CHESS_APP_HAVE_AVAHI)
    chess_discovery_avahi_stop(ctx);
#elif defined(CHESS_APP_HAVE_DNSSD)
    chess_discovery_dnssd_stop(ctx);
#endif

    memset(ctx, 0, sizeof(*ctx));
}

bool chess_discovery_poll(ChessDiscoveryContext *ctx, ChessDiscoveredPeer *out_remote_peer)
{
    bool readable[CHESS_DISCOVERY_MAX_POLL_FDS];
    int fds[CHESS_DISCOVERY_MAX_POLL_FDS];
    int fd_count;

    if (!ctx || !ctx->started || !out_remote_peer) {
        return false;
    }

    /* Self-contained pump: get fds, poll them, process events */
    fd_count = chess_discovery_get_poll_fds(ctx, fds, CHESS_DISCOVERY_MAX_POLL_FDS);
    memset(readable, 0, sizeof(readable));
    if (fd_count > 0) {
        struct pollfd pfds[CHESS_DISCOVERY_MAX_POLL_FDS];
        int i;
        for (i = 0; i < fd_count; ++i) {
            pfds[i].fd = fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        if (poll(pfds, (nfds_t)fd_count, 5) > 0) {
            for (i = 0; i < fd_count; ++i) {
                readable[i] = (pfds[i].revents & POLLIN) != 0;
            }
        }
    }

    chess_discovery_process_events(ctx, readable, fd_count);
    return chess_discovery_check_result(ctx, out_remote_peer);
}

int chess_discovery_get_poll_fds(ChessDiscoveryContext *ctx, int *out_fds, int max_fds)
{
    if (!ctx || !ctx->started || !out_fds || max_fds <= 0 || !ctx->platform) {
        return 0;
    }

#if defined(CHESS_APP_HAVE_DNSSD)
    return chess_discovery_dnssd_get_poll_fds(ctx, out_fds, max_fds);
#elif defined(CHESS_APP_HAVE_AVAHI)
    return chess_discovery_avahi_get_poll_fds(ctx, out_fds, max_fds);
#else
    (void)out_fds; (void)max_fds;
    return 0;
#endif
}

void chess_discovery_process_events(ChessDiscoveryContext *ctx, const bool *readable, int fd_count)
{
    if (!ctx || !ctx->started) {
        return;
    }

#if defined(CHESS_APP_HAVE_DNSSD)
    chess_discovery_dnssd_process_events(ctx, readable, fd_count);
#elif defined(CHESS_APP_HAVE_AVAHI)
    chess_discovery_avahi_process_events(ctx, readable, fd_count);
#else
    (void)readable; (void)fd_count;
#endif
}

bool chess_discovery_check_result(ChessDiscoveryContext *ctx, ChessDiscoveredPeer *out_remote_peer)
{
    if (!ctx || !ctx->started || !out_remote_peer) {
        return false;
    }

#if defined(CHESS_APP_HAVE_AVAHI)
    return chess_discovery_avahi_check_result(ctx, out_remote_peer);
#elif defined(CHESS_APP_HAVE_DNSSD)
    return chess_discovery_dnssd_check_result(ctx, out_remote_peer);
#else
    {
        const char *ip       = getenv("CHESS_REMOTE_IP");
        const char *profile  = getenv("CHESS_REMOTE_PROFILE_ID");
        const char *port_str = getenv("CHESS_REMOTE_PORT");
        char *endptr     = NULL;
        long  parsed_port = 0;

        if (!ip || !profile || !port_str) {
            return false;
        }

        if (!chess_parse_ipv4(ip, &out_remote_peer->tcp_ipv4)) {
            SDL_Log("Ignoring CHESS_REMOTE_IP: invalid IPv4 '%s'", ip);
            return false;
        }

        errno = 0;
        parsed_port = strtol(port_str, &endptr, 10);
        if (errno != 0 || endptr == port_str || *endptr != '\0' || parsed_port <= 0 || parsed_port > 65535) {
            SDL_Log("Ignoring CHESS_REMOTE_PORT: invalid port '%s'", port_str);
            return false;
        }

        SDL_strlcpy(out_remote_peer->peer.profile_id, profile, sizeof(out_remote_peer->peer.profile_id));
        out_remote_peer->tcp_port = (uint16_t)parsed_port;

        if (SDL_strncmp(out_remote_peer->peer.profile_id, ctx->local_peer.profile_id, CHESS_PROFILE_ID_STRING_LEN) == 0) {
            SDL_Log("Ignoring discovered peer because profile_id matches local peer");
            return false;
        }

        return true;
    }
#endif
}

bool chess_discovery_poll_removal(ChessDiscoveryContext *ctx, char *out_profile_id, size_t out_size)
{
    if (!ctx || !out_profile_id || out_size == 0 || ctx->removal_count <= 0) {
        return false;
    }

    SDL_strlcpy(out_profile_id, ctx->removal_queue[0], out_size);

    /* Shift remaining entries forward */
    for (int i = 1; i < ctx->removal_count; i++) {
        SDL_strlcpy(ctx->removal_queue[i - 1], ctx->removal_queue[i],
                    CHESS_PROFILE_ID_STRING_LEN);
    }
    ctx->removal_count--;
    return true;
}
