#include "chess_app/network_discovery.h"

#include "chess_app/network_peer.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool chess_discovery_start(ChessDiscoveryContext *ctx, const ChessPeerInfo *local_peer, uint16_t game_port)
{
    if (!ctx || !local_peer) {
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->started = true;
    ctx->game_port = game_port;
    ctx->local_peer = *local_peer;

#if defined(CHESS_APP_HAVE_AVAHI)
    SDL_Log(
        "mDNS discovery backend enabled (Avahi module detected at build time), advertising TCP port %u.",
        (unsigned int)ctx->game_port
    );
#else
    SDL_Log(
        "mDNS discovery backend unavailable, using env simulation. Local TCP port is %u. Set CHESS_REMOTE_IP and CHESS_REMOTE_UUID.",
        (unsigned int)ctx->game_port
    );
#endif

    return true;
}

void chess_discovery_stop(ChessDiscoveryContext *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

bool chess_discovery_poll(ChessDiscoveryContext *ctx, ChessDiscoveredPeer *out_remote_peer)
{
    if (!ctx || !ctx->started || !out_remote_peer || ctx->remote_emitted) {
        return false;
    }

#if defined(CHESS_APP_HAVE_AVAHI)
    /* Real mDNS browse/publish integration will be connected here next. */
    (void)out_remote_peer;
    return false;
#else
    const char *ip = getenv("CHESS_REMOTE_IP");
    const char *uuid = getenv("CHESS_REMOTE_UUID");
    const char *port_str = getenv("CHESS_REMOTE_PORT");
    char *endptr = NULL;
    long parsed_port = 0;

    if (!ip || !uuid || !port_str) {
        return false;
    }

    if (!chess_parse_ipv4(ip, &out_remote_peer->peer.ipv4_host_order)) {
        SDL_Log("Ignoring CHESS_REMOTE_IP: invalid IPv4 '%s'", ip);
        ctx->remote_emitted = true;
        return false;
    }

    errno = 0;
    parsed_port = strtol(port_str, &endptr, 10);
    if (errno != 0 || endptr == port_str || *endptr != '\0' || parsed_port <= 0 || parsed_port > 65535) {
        SDL_Log("Ignoring CHESS_REMOTE_PORT: invalid port '%s'", port_str);
        ctx->remote_emitted = true;
        return false;
    }

    SDL_strlcpy(out_remote_peer->peer.uuid, uuid, sizeof(out_remote_peer->peer.uuid));
    out_remote_peer->tcp_port = (uint16_t)parsed_port;

    if (SDL_strncmp(out_remote_peer->peer.uuid, ctx->local_peer.uuid, CHESS_UUID_STRING_LEN) == 0) {
        SDL_Log("Ignoring discovered peer because UUID matches local peer");
        ctx->remote_emitted = true;
        return false;
    }

    ctx->remote_emitted = true;
    return true;
#endif
}
