/* ============================================================
 * DNS-SD (Bonjour) backend — macOS
 *
 * Backend implementation for mDNS/DNS-SD peer discovery using
 * Apple's dns_sd.h API.  Compiled to an empty translation unit
 * when CHESS_APP_HAVE_DNSSD is not defined.
 * ============================================================ */

#include "discovery_internal.h"

#if defined(CHESS_APP_HAVE_DNSSD)

#include "chess_app/network_peer.h"

#include <SDL3/SDL.h>
#include <dns_sd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHESS_DNSSD_SERVICE_TYPE "_chess._tcp"
#define CHESS_DNSSD_PENDING_MAX 16

typedef struct {
    char     service_name[CHESS_PROFILE_ID_STRING_LEN];
    char     regtype[64];
    char     reply_domain[64];
    uint32_t interface_index;
} ChessDnssdPendingService;

typedef struct {
    DNSServiceRef register_ref;
    DNSServiceRef browse_ref;
    DNSServiceRef resolve_ref;
    DNSServiceRef addr_ref;
    bool          resolve_done;       /* set by resolve_callback, consumed in poll */
    bool          pending_peer_ready;
    ChessDiscoveredPeer pending_peer;
    char          resolving_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    uint64_t      resolve_started_at; /* SDL_GetTicks() when resolve began */
    ChessDnssdPendingService pending_queue[CHESS_DNSSD_PENDING_MAX];
    int                      pending_count;
} ChessDnssdContext;

#define CHESS_DNSSD_RESOLVE_TIMEOUT_MS 5000

/* ------------------------------------------------------------------ */
/*  Static helpers                                                     */
/* ------------------------------------------------------------------ */

static void txt_copy_to_buffer(
    const unsigned char *txt_record,
    uint16_t txt_len,
    const char *key,
    char *out,
    size_t out_size)
{
    const void *value = NULL;
    uint8_t value_len = 0;

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!txt_record || txt_len == 0 || !key) {
        return;
    }

    value = TXTRecordGetValuePtr(txt_len, txt_record, key, &value_len);
    if (!value || value_len == 0) {
        return;
    }

    if ((size_t)value_len >= out_size) {
        value_len = (uint8_t)(out_size - 1u);
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
}

static void fill_peer_identity_from_txt(
    ChessPeerInfo *peer,
    uint16_t txt_len,
    const unsigned char *txt_record)
{
    char user[CHESS_PEER_USERNAME_MAX_LEN];
    char host[CHESS_PEER_HOSTNAME_MAX_LEN];
    char profile_id[CHESS_PROFILE_ID_STRING_LEN];

    if (!peer) {
        return;
    }

    user[0] = '\0';
    host[0] = '\0';
    profile_id[0] = '\0';

    txt_copy_to_buffer(txt_record, txt_len, "user", user, sizeof(user));
    txt_copy_to_buffer(txt_record, txt_len, "host", host, sizeof(host));
    txt_copy_to_buffer(txt_record, txt_len, "profile", profile_id, sizeof(profile_id));

    if (user[0] != '\0' || host[0] != '\0') {
        chess_peer_set_identity_tokens(peer, user, host);
    }
    if (profile_id[0] != '\0') {
        SDL_strlcpy(peer->profile_id, profile_id, sizeof(peer->profile_id));
    }
}

/* ------------------------------------------------------------------ */
/*  DNS-SD callbacks                                                   */
/* ------------------------------------------------------------------ */

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
            ctx->local_ipv4 = ip;
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

    /* If DNS-SD resolved to loopback the remote service is on the same machine,
     * substitute with our own LAN IP so both peers see the same address. */
    if ((resolved_ip >> 24) == 127) {
        resolved_ip = ctx->local_ipv4;
    }
    dnssd->pending_peer.tcp_ipv4 = resolved_ip;
    SDL_strlcpy(dnssd->pending_peer.peer.profile_id, dnssd->resolving_profile_id,
                sizeof(dnssd->pending_peer.peer.profile_id));
    dnssd->pending_peer_ready = true;
    SDL_Log("DNS-SD: peer ready — profile_id=%s port=%u",
            dnssd->resolving_profile_id, (unsigned)dnssd->pending_peer.tcp_port);
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

    fill_peer_identity_from_txt(&dnssd->pending_peer.peer, txt_len, txt_record);
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
        /* Service removed — notify lobby.
         * Strip optional ":port" suffix so that Avahi-style service names
         * ("profile_id:port") are matched against the clean profile_id
         * stored in the lobby.  Pure DNS-SD names have no colon. */
        if (service_name && service_name[0] != '\0') {
            char clean_id[CHESS_PROFILE_ID_STRING_LEN];
            const char *colon = strchr(service_name, ':');
            size_t id_len = colon ? (size_t)(colon - service_name) : strlen(service_name);
            if (id_len >= sizeof(clean_id)) {
                id_len = sizeof(clean_id) - 1;
            }
            memcpy(clean_id, service_name, id_len);
            clean_id[id_len] = '\0';

            if (SDL_strcmp(clean_id, ctx->local_peer.profile_id) != 0 &&
                ctx->removal_count < CHESS_DISCOVERY_REMOVAL_QUEUE_MAX) {
                /* Deduplicate: skip if already queued */
                bool already_queued = false;
                for (int q = 0; q < ctx->removal_count; q++) {
                    if (SDL_strcmp(ctx->removal_queue[q], clean_id) == 0) {
                        already_queued = true;
                        break;
                    }
                }
                if (!already_queued) {
                    SDL_strlcpy(ctx->removal_queue[ctx->removal_count],
                                clean_id, CHESS_PROFILE_ID_STRING_LEN);
                    ctx->removal_count++;
                }
            }
        }
        SDL_Log("DNS-SD: service '%s' removed", service_name);
        return;
    }

    /* Skip our own advertisement — use exact match so a Bonjour-renamed
     * duplicate like "<uuid> (2)" is not mistakenly filtered. */
    if (SDL_strcmp(service_name, ctx->local_peer.profile_id) == 0) {
        SDL_Log("DNS-SD: skipping own service");
        return;
    }

    /* Skip if this service is already being resolved or queued */
    if (SDL_strcmp(service_name, dnssd->resolving_profile_id) == 0) {
        SDL_Log("DNS-SD: already resolving or peer found, skipping '%s'", service_name);
        return;
    }
    for (int i = 0; i < dnssd->pending_count; i++) {
        if (SDL_strcmp(dnssd->pending_queue[i].service_name, service_name) == 0) {
            SDL_Log("DNS-SD: already resolving or peer found, skipping '%s'", service_name);
            return;
        }
    }

    /* If a resolve is in progress or a result is pending, queue for later */
    if (dnssd->resolve_ref || dnssd->pending_peer_ready) {
        if (dnssd->pending_count < CHESS_DNSSD_PENDING_MAX) {
            ChessDnssdPendingService *ps = &dnssd->pending_queue[dnssd->pending_count++];
            SDL_strlcpy(ps->service_name, service_name, sizeof(ps->service_name));
            SDL_strlcpy(ps->regtype, regtype, sizeof(ps->regtype));
            SDL_strlcpy(ps->reply_domain, reply_domain, sizeof(ps->reply_domain));
            ps->interface_index = interface_index;
            SDL_Log("DNS-SD: queued peer service '%s' for later resolve", service_name);
        } else {
            SDL_Log("DNS-SD: pending queue full, skipping '%s'", service_name);
        }
        return;
    }

    SDL_Log("DNS-SD: found peer service '%s', resolving...", service_name);
    memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
    SDL_strlcpy(dnssd->pending_peer.peer.profile_id, service_name,
                sizeof(dnssd->pending_peer.peer.profile_id));
    SDL_strlcpy(dnssd->resolving_profile_id, service_name, sizeof(dnssd->resolving_profile_id));
    dnssd->resolve_started_at = SDL_GetTicks();

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

/* ------------------------------------------------------------------ */
/*  Backend API                                                        */
/* ------------------------------------------------------------------ */

bool chess_discovery_dnssd_start(ChessDiscoveryContext *ctx)
{
    ChessDnssdContext  *dnssd = (ChessDnssdContext *)calloc(1, sizeof(ChessDnssdContext));
    DNSServiceErrorType err;
    TXTRecordRef txt_record;

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
                struct pollfd pfd = { .fd = self_fd, .events = POLLIN, .revents = 0 };
                if (poll(&pfd, 1, 500) > 0) {
                    DNSServiceProcessResult(self_ref);
                }
            }
            DNSServiceRefDeallocate(self_ref);
        }

        if (ctx->local_ipv4 != 0) {
            SDL_Log("DNS-SD: local IP resolved to %u.%u.%u.%u",
                    (ctx->local_ipv4 >> 24) & 0xffu,
                    (ctx->local_ipv4 >> 16) & 0xffu,
                    (ctx->local_ipv4 >>  8) & 0xffu,
                     ctx->local_ipv4        & 0xffu);
        } else {
            SDL_Log("DNS-SD: could not resolve local IP");
        }
    }

    TXTRecordCreate(&txt_record, 0, NULL);
    TXTRecordSetValue(&txt_record,
                      "user",
                      (uint8_t)SDL_strlen(ctx->local_peer.username),
                      ctx->local_peer.username);
    TXTRecordSetValue(&txt_record,
                      "host",
                      (uint8_t)SDL_strlen(ctx->local_peer.hostname),
                      ctx->local_peer.hostname);
    TXTRecordSetValue(&txt_record,
              "profile",
              (uint8_t)SDL_strlen(ctx->local_peer.profile_id),
              ctx->local_peer.profile_id);

    err = DNSServiceRegister(
        &dnssd->register_ref,
        0, 0,
        ctx->local_peer.profile_id,
        CHESS_DNSSD_SERVICE_TYPE,
        NULL, NULL,
        htons(ctx->game_port),
        TXTRecordGetLength(&txt_record), TXTRecordGetBytesPtr(&txt_record),
        register_callback,
        ctx);
    TXTRecordDeallocate(&txt_record);
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
    return true;
}

void chess_discovery_dnssd_stop(ChessDiscoveryContext *ctx)
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

int chess_discovery_dnssd_get_poll_fds(ChessDiscoveryContext *ctx,
                                       int *out_fds, int max_fds)
{
    ChessDnssdContext *dnssd = (ChessDnssdContext *)ctx->platform;
    DNSServiceRef refs[4];
    int count = 0;
    int i;

    refs[0] = dnssd->register_ref;
    refs[1] = dnssd->browse_ref;
    refs[2] = dnssd->resolve_ref;
    refs[3] = dnssd->addr_ref;

    for (i = 0; i < 4 && count < max_fds; ++i) {
        if (refs[i]) {
            int fd = DNSServiceRefSockFD(refs[i]);
            if (fd >= 0) {
                out_fds[count++] = fd;
            }
        }
    }
    return count;
}

void chess_discovery_dnssd_process_events(ChessDiscoveryContext *ctx,
                                          const bool *readable, int fd_count)
{
    ChessDnssdContext *dnssd = (ChessDnssdContext *)ctx->platform;
    int idx = 0;

    if (!dnssd) {
        return;
    }

    /* Process refs in the same order as get_poll_fds enumerated them */
    if (dnssd->register_ref && DNSServiceRefSockFD(dnssd->register_ref) >= 0) {
        if (idx < fd_count && readable && readable[idx]) {
            DNSServiceProcessResult(dnssd->register_ref);
        }
        idx++;
    }

    if (dnssd->browse_ref && DNSServiceRefSockFD(dnssd->browse_ref) >= 0) {
        if (idx < fd_count && readable && readable[idx]) {
            DNSServiceProcessResult(dnssd->browse_ref);
        }
        idx++;
    }

    if (dnssd->resolve_ref && DNSServiceRefSockFD(dnssd->resolve_ref) >= 0) {
        if (idx < fd_count && readable && readable[idx]) {
            DNSServiceProcessResult(dnssd->resolve_ref);
        }
        idx++;
    }
    /* Clean up completed resolve (resolve_done set by callback) */
    if (dnssd->resolve_ref && dnssd->resolve_done) {
        DNSServiceRefDeallocate(dnssd->resolve_ref);
        dnssd->resolve_ref  = NULL;
        dnssd->resolve_done = false;
    }

    if (dnssd->addr_ref && DNSServiceRefSockFD(dnssd->addr_ref) >= 0) {
        if (idx < fd_count && readable && readable[idx]) {
            DNSServiceProcessResult(dnssd->addr_ref);
        }
        idx++;
    }
}

bool chess_discovery_dnssd_check_result(ChessDiscoveryContext *ctx,
                                        ChessDiscoveredPeer *out_remote_peer)
{
    ChessDnssdContext *dnssd = (ChessDnssdContext *)ctx->platform;
    if (!dnssd) {
        return false;
    }

    /* Detect stalled resolve pipeline */
    if (!dnssd->pending_peer_ready &&
        dnssd->resolving_profile_id[0] != '\0' &&
        SDL_GetTicks() - dnssd->resolve_started_at > CHESS_DNSSD_RESOLVE_TIMEOUT_MS) {
        SDL_Log("DNS-SD: resolve timed out for '%s'", dnssd->resolving_profile_id);
        if (dnssd->resolve_ref) {
            DNSServiceRefDeallocate(dnssd->resolve_ref);
            dnssd->resolve_ref  = NULL;
            dnssd->resolve_done = false;
        }
        if (dnssd->addr_ref) {
            DNSServiceRefDeallocate(dnssd->addr_ref);
            dnssd->addr_ref = NULL;
        }
        dnssd->resolving_profile_id[0] = '\0';
        memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
    }
    /* Immediate stall: resolve finished but no addr started */
    if (!dnssd->pending_peer_ready &&
        !dnssd->resolve_ref && !dnssd->addr_ref &&
        dnssd->resolving_profile_id[0] != '\0') {
        SDL_Log("DNS-SD: resolve stalled for '%s', skipping", dnssd->resolving_profile_id);
        dnssd->resolving_profile_id[0] = '\0';
        memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
    }

    /* Advance: either a peer was resolved, or pipeline was cleared */
    if (dnssd->pending_peer_ready ||
        (!dnssd->resolve_ref && !dnssd->addr_ref &&
         dnssd->resolving_profile_id[0] == '\0' && dnssd->pending_count > 0)) {

        bool has_result = dnssd->pending_peer_ready;
        if (has_result) {
            *out_remote_peer = dnssd->pending_peer;
            dnssd->pending_peer_ready = false;
            memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
            dnssd->resolving_profile_id[0] = '\0';
        }

        /* Start resolving the next queued service, if any */
        while (dnssd->pending_count > 0) {
            ChessDnssdPendingService ps = dnssd->pending_queue[0];
            DNSServiceErrorType rerr;
            dnssd->pending_count--;
            if (dnssd->pending_count > 0) {
                memmove(&dnssd->pending_queue[0], &dnssd->pending_queue[1],
                        (size_t)dnssd->pending_count * sizeof(dnssd->pending_queue[0]));
            }

            SDL_Log("DNS-SD: found peer service '%s', resolving...", ps.service_name);
            memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
            SDL_strlcpy(dnssd->pending_peer.peer.profile_id, ps.service_name,
                        sizeof(dnssd->pending_peer.peer.profile_id));
            SDL_strlcpy(dnssd->resolving_profile_id, ps.service_name,
                        sizeof(dnssd->resolving_profile_id));
            dnssd->resolve_started_at = SDL_GetTicks();

            rerr = DNSServiceResolve(
                &dnssd->resolve_ref,
                0,
                ps.interface_index,
                ps.service_name,
                ps.regtype,
                ps.reply_domain,
                resolve_callback,
                ctx);
            if (rerr == kDNSServiceErr_NoError) {
                break;
            }
            SDL_Log("DNS-SD: DNSServiceResolve failed: %d", rerr);
            dnssd->resolve_ref = NULL;
            dnssd->resolving_profile_id[0] = '\0';
        }

        return has_result;
    }

    return false;
}

#endif /* CHESS_APP_HAVE_DNSSD */
