#include "chess_app/network_discovery.h"

#include "chess_app/network_peer.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * DNS-SD (Bonjour) backend — macOS
 * ============================================================ */

#if defined(CHESS_APP_HAVE_DNSSD)

#include <dns_sd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <unistd.h>

#define CHESS_DNSSD_SERVICE_TYPE "_chess._tcp"

typedef struct {
    DNSServiceRef register_ref;
    DNSServiceRef browse_ref;
    DNSServiceRef resolve_ref;
    DNSServiceRef addr_ref;
    bool          resolve_done;       /* set by resolve_callback, consumed in poll */
    bool          pending_peer_ready;
    ChessDiscoveredPeer pending_peer;
    char          resolving_uuid[CHESS_UUID_STRING_LEN];
} ChessDnssdContext;

/* Pump one DNS-SD service ref with a near-zero timeout (5 ms). */
static bool dnssd_pump(DNSServiceRef ref)
{
    int fd;
    fd_set rfds;
    struct timeval tv = {0, 5000};

    fd = DNSServiceRefSockFD(ref);
    if (fd < 0) {
        return false;
    }

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        return DNSServiceProcessResult(ref) == kDNSServiceErr_NoError;
    }

    return true;
}

static void self_addr_callback(
    DNSServiceRef           ref,
    DNSServiceFlags         flags,
    uint32_t                interface_index,
    DNSServiceErrorType     error,
    const char             *hostname,
    const struct sockaddr  *address,
    uint32_t                ttl,
    void                   *context)
{
    ChessDiscoveryContext *ctx = (ChessDiscoveryContext *)context;

    (void)ref; (void)flags; (void)interface_index; (void)hostname; (void)ttl;

    if (error != kDNSServiceErr_NoError || !address) {
        return;
    }
    if (address->sa_family != AF_INET) {
        return;
    }

    {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)address;
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if ((ip >> 24) != 127) { /* ignore loopback, keep LAN IP */
            ctx->local_peer.ipv4_host_order = ip;
        }
    }
}

static void addr_callback(
    DNSServiceRef           ref,
    DNSServiceFlags         flags,
    uint32_t                interface_index,
    DNSServiceErrorType     error,
    const char             *hostname,
    const struct sockaddr  *address,
    uint32_t                ttl,
    void                   *context)
{
    ChessDiscoveryContext *ctx   = (ChessDiscoveryContext *)context;
    ChessDnssdContext     *dnssd = (ChessDnssdContext *)ctx->platform;

    (void)ref; (void)flags; (void)interface_index; (void)hostname; (void)ttl;

    if (error != kDNSServiceErr_NoError || !address || dnssd->pending_peer_ready) {
        return;
    }
    if (address->sa_family != AF_INET) {
        return;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)address;
    uint32_t resolved_ip = ntohl(sin->sin_addr.s_addr);

    /* If DNS-SD resolved to loopback the remote service is on the same machine.
     * Substitute with our own LAN IP so both sides compare equal and the UUID
     * tiebreaker decides the role (instead of both becoming CLIENT). */
    if ((resolved_ip >> 24) == 127) {
        resolved_ip = ctx->local_peer.ipv4_host_order;
    }
    dnssd->pending_peer.peer.ipv4_host_order = resolved_ip;
    SDL_strlcpy(dnssd->pending_peer.peer.uuid, dnssd->resolving_uuid,
                sizeof(dnssd->pending_peer.peer.uuid));
    dnssd->pending_peer_ready = true;
    SDL_Log("DNS-SD: peer ready — uuid=%s port=%u",
            dnssd->resolving_uuid, (unsigned)dnssd->pending_peer.tcp_port);
}

static void resolve_callback(
    DNSServiceRef        ref,
    DNSServiceFlags      flags,
    uint32_t             interface_index,
    DNSServiceErrorType  error,
    const char          *fullname,
    const char          *host_target,
    uint16_t             port,          /* network byte order */
    uint16_t             txt_len,
    const unsigned char *txt_record,
    void                *context)
{
    ChessDiscoveryContext *ctx   = (ChessDiscoveryContext *)context;
    ChessDnssdContext     *dnssd = (ChessDnssdContext *)ctx->platform;
    DNSServiceErrorType    err;

    (void)ref; (void)flags; (void)fullname; (void)txt_len; (void)txt_record;

    /* Signal poll() to deallocate resolve_ref regardless of outcome */
    dnssd->resolve_done = true;

    if (error != kDNSServiceErr_NoError) {
        SDL_Log("DNS-SD: resolve error %d", error);
        return;
    }

    dnssd->pending_peer.tcp_port = ntohs(port);

    err = DNSServiceGetAddrInfo(
        &dnssd->addr_ref,
        0,
        interface_index,
        kDNSServiceProtocol_IPv4,
        host_target,
        addr_callback,
        ctx);
    if (err != kDNSServiceErr_NoError) {
        SDL_Log("DNS-SD: DNSServiceGetAddrInfo failed: %d", err);
        dnssd->addr_ref = NULL;
    }
}

static void browse_callback(
    DNSServiceRef        ref,
    DNSServiceFlags      flags,
    uint32_t             interface_index,
    DNSServiceErrorType  error,
    const char          *service_name,
    const char          *regtype,
    const char          *reply_domain,
    void                *context)
{
    ChessDiscoveryContext *ctx   = (ChessDiscoveryContext *)context;
    ChessDnssdContext     *dnssd = (ChessDnssdContext *)ctx->platform;
    DNSServiceErrorType    err;

    (void)ref;

    if (error != kDNSServiceErr_NoError) {
        SDL_Log("DNS-SD: browse error %d", error);
        return;
    }
    if (!(flags & kDNSServiceFlagsAdd)) {
        return; /* service removal — ignore */
    }

    /* Skip our own advertisement */
    if (SDL_strncmp(service_name, ctx->local_peer.uuid, CHESS_UUID_STRING_LEN - 1) == 0) {
        SDL_Log("DNS-SD: skipping own service");
        return;
    }

    /* Skip if already in a resolve/addr pipeline or peer already found */
    if (dnssd->resolve_ref || dnssd->pending_peer_ready) {
        SDL_Log("DNS-SD: already resolving or peer found, skipping '%s'", service_name);
        return;
    }

    SDL_Log("DNS-SD: found peer service '%s', resolving...", service_name);
    SDL_strlcpy(dnssd->resolving_uuid, service_name, sizeof(dnssd->resolving_uuid));

    err = DNSServiceResolve(
        &dnssd->resolve_ref,
        0,
        interface_index,
        service_name,
        regtype,
        reply_domain,
        resolve_callback,
        ctx);
    if (err != kDNSServiceErr_NoError) {
        SDL_Log("DNS-SD: DNSServiceResolve failed: %d", err);
        dnssd->resolve_ref = NULL;
    }
}

static void register_callback(
    DNSServiceRef        ref,
    DNSServiceFlags      flags,
    DNSServiceErrorType  error,
    const char          *name,
    const char          *regtype,
    const char          *domain,
    void                *context)
{
    (void)ref; (void)flags; (void)context;
    if (error == kDNSServiceErr_NoError) {
        SDL_Log("DNS-SD: registered '%s.%s%s'", name, regtype, domain);
    } else {
        SDL_Log("DNS-SD: registration error %d", error);
    }
}

#endif /* CHESS_APP_HAVE_DNSSD */

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
    SDL_Log(
        "mDNS discovery backend enabled (Avahi), advertising TCP port %u.",
        (unsigned int)ctx->game_port);
#elif defined(CHESS_APP_HAVE_DNSSD)
    {
        ChessDnssdContext  *dnssd = (ChessDnssdContext *)calloc(1, sizeof(ChessDnssdContext));
        DNSServiceErrorType err;

        if (!dnssd) {
            SDL_Log("DNS-SD: failed to allocate context");
            return false;
        }
        ctx->platform = dnssd;

        /* Resolve our own IP via DNS-SD — same source as remote peer resolution,
         * so IP comparison in election is always symmetric (no getifaddrs). */
        {
            char hostname[256];
            DNSServiceRef self_ref = NULL;
            DNSServiceErrorType self_err;

            hostname[0] = '\0';
            gethostname(hostname, sizeof(hostname) - 1);
            if (strstr(hostname, ".local") == NULL) {
                size_t hlen = strlen(hostname);
                if (hlen + 6 < sizeof(hostname)) {
                    memcpy(hostname + hlen, ".local", 7);
                }
            }

            self_err = DNSServiceGetAddrInfo(
                &self_ref, 0, 0,
                kDNSServiceProtocol_IPv4,
                hostname,
                self_addr_callback,
                ctx);
            if (self_err == kDNSServiceErr_NoError) {
                int self_fd = DNSServiceRefSockFD(self_ref);
                if (self_fd >= 0) {
                    struct timeval tv = {0, 500000}; /* 500 ms */
                    fd_set rfds;
                    FD_ZERO(&rfds);
                    FD_SET(self_fd, &rfds);
                    if (select(self_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
                        DNSServiceProcessResult(self_ref);
                    }
                }
                DNSServiceRefDeallocate(self_ref);
            }

            if (ctx->local_peer.ipv4_host_order != 0) {
                local_peer->ipv4_host_order = ctx->local_peer.ipv4_host_order;
                SDL_Log("DNS-SD: local IP resolved to %u.%u.%u.%u",
                        (ctx->local_peer.ipv4_host_order >> 24) & 0xffu,
                        (ctx->local_peer.ipv4_host_order >> 16) & 0xffu,
                        (ctx->local_peer.ipv4_host_order >>  8) & 0xffu,
                         ctx->local_peer.ipv4_host_order        & 0xffu);
            } else {
                SDL_Log("DNS-SD: could not resolve local IP, election will use UUID only");
            }
        }

        err = DNSServiceRegister(
            &dnssd->register_ref,
            0, 0,
            ctx->local_peer.uuid,
            CHESS_DNSSD_SERVICE_TYPE,
            NULL, NULL,
            htons(ctx->game_port),
            0, NULL,
            register_callback,
            ctx);
        if (err != kDNSServiceErr_NoError) {
            SDL_Log("DNS-SD: DNSServiceRegister failed: %d", err);
            free(dnssd);
            ctx->platform = NULL;
            return false;
        }

        err = DNSServiceBrowse(
            &dnssd->browse_ref,
            0, 0,
            CHESS_DNSSD_SERVICE_TYPE,
            NULL,
            browse_callback,
            ctx);
        if (err != kDNSServiceErr_NoError) {
            SDL_Log("DNS-SD: DNSServiceBrowse failed: %d", err);
            DNSServiceRefDeallocate(dnssd->register_ref);
            free(dnssd);
            ctx->platform = NULL;
            return false;
        }

        SDL_Log("DNS-SD: advertising '%s' and browsing for peers on port %u",
                CHESS_DNSSD_SERVICE_TYPE, (unsigned)ctx->game_port);
    }
#else
    SDL_Log(
        "mDNS discovery backend unavailable, using env simulation. Local TCP port is %u."
        " Set CHESS_REMOTE_IP, CHESS_REMOTE_PORT and CHESS_REMOTE_UUID.",
        (unsigned int)ctx->game_port);
#endif

    return true;
}

void chess_discovery_stop(ChessDiscoveryContext *ctx)
{
    if (!ctx) {
        return;
    }

#if defined(CHESS_APP_HAVE_DNSSD)
    {
        ChessDnssdContext *dnssd = (ChessDnssdContext *)ctx->platform;
        if (dnssd) {
            if (dnssd->addr_ref)     { DNSServiceRefDeallocate(dnssd->addr_ref);     }
            if (dnssd->resolve_ref)  { DNSServiceRefDeallocate(dnssd->resolve_ref);  }
            if (dnssd->browse_ref)   { DNSServiceRefDeallocate(dnssd->browse_ref);   }
            if (dnssd->register_ref) { DNSServiceRefDeallocate(dnssd->register_ref); }
            free(dnssd);
        }
    }
#endif

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
#elif defined(CHESS_APP_HAVE_DNSSD)
    {
        ChessDnssdContext *dnssd = (ChessDnssdContext *)ctx->platform;
        if (!dnssd) {
            return false;
        }

        dnssd_pump(dnssd->register_ref);
        dnssd_pump(dnssd->browse_ref);

        if (dnssd->resolve_ref) {
            dnssd_pump(dnssd->resolve_ref);
            if (dnssd->resolve_done) {
                DNSServiceRefDeallocate(dnssd->resolve_ref);
                dnssd->resolve_ref  = NULL;
                dnssd->resolve_done = false;
            }
        }

        if (dnssd->addr_ref) {
            dnssd_pump(dnssd->addr_ref);
        }

        if (dnssd->pending_peer_ready) {
            *out_remote_peer = dnssd->pending_peer;
            ctx->remote_emitted = true;
            return true;
        }

        return false;
    }
#else
    {
        const char *ip       = getenv("CHESS_REMOTE_IP");
        const char *uuid     = getenv("CHESS_REMOTE_UUID");
        const char *port_str = getenv("CHESS_REMOTE_PORT");
        char *endptr     = NULL;
        long  parsed_port = 0;

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
    }
#endif
}
