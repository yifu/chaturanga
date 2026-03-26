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
#include <poll.h>
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

/* Pump one DNS-SD service ref with a near-zero timeout (5 ms). */
static bool dnssd_pump(DNSServiceRef ref)
{
    int fd;
    struct pollfd pfd;

    fd = DNSServiceRefSockFD(ref);
    if (fd < 0) {
        return false;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, 5) > 0) {
        const bool ok = DNSServiceProcessResult(ref) == kDNSServiceErr_NoError;
        return ok;
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

#endif /* CHESS_APP_HAVE_DNSSD */


/* ============================================================
 * Avahi backend — Linux
 * ============================================================ */

#if defined(CHESS_APP_HAVE_AVAHI)

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>

#define CHESS_AVAHI_SERVICE_TYPE "_chess._tcp"
#define CHESS_AVAHI_PENDING_MAX 16

typedef struct {
    char           name[CHESS_PROFILE_ID_STRING_LEN + 8];
    char           type[64];
    char           domain[64];
    AvahiIfIndex   interface;
    AvahiProtocol  protocol;
} ChessAvahiPendingService;

typedef struct {
    AvahiSimplePoll     *simple_poll;
    AvahiClient         *client;
    AvahiEntryGroup     *group;
    AvahiServiceBrowser *browser;
    bool                 pending_peer_ready;
    bool                 resolving;          /* resolver in flight */
    ChessDiscoveredPeer  pending_peer;
    char                 resolving_profile_id[CHESS_PROFILE_ID_STRING_LEN + 8];
    char                 service_name[CHESS_PROFILE_ID_STRING_LEN + 8]; /* "uuid:port" */
    uint64_t             resolve_started_at;
    ChessAvahiPendingService pending_queue[CHESS_AVAHI_PENDING_MAX];
    int                      pending_count;
} ChessAvahiContext;

#define CHESS_AVAHI_RESOLVE_TIMEOUT_MS 5000

static uint32_t avahi_resolve_local_ip(void)
{
    struct ifaddrs *ifaddr, *ifa;
    uint32_t result = 0;

    if (getifaddrs(&ifaddr) == -1) {
        return 0;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!(ifa->ifa_flags & IFF_UP)) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }

        const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if ((ip >> 24) != 127) {
            result = ip;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return result;
}

static void avahi_txt_copy_to_buffer(
    AvahiStringList *txt,
    const char *key,
    char *out,
    size_t out_size)
{
    AvahiStringList *entry;
    char *value = NULL;
    size_t value_len = 0;

    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!txt || !key) {
        return;
    }

    entry = avahi_string_list_find(txt, key);
    if (!entry) {
        return;
    }

    if (avahi_string_list_get_pair(entry, NULL, &value, &value_len) < 0 ||
        !value || value_len == 0) {
        avahi_free(value);
        return;
    }

    if (value_len >= out_size) {
        value_len = out_size - 1;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    avahi_free(value);
}

static void avahi_fill_peer_identity_from_txt(
    ChessPeerInfo *peer,
    AvahiStringList *txt)
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

    avahi_txt_copy_to_buffer(txt, "user", user, sizeof(user));
    avahi_txt_copy_to_buffer(txt, "host", host, sizeof(host));
    avahi_txt_copy_to_buffer(txt, "profile", profile_id, sizeof(profile_id));

    if (user[0] != '\0' || host[0] != '\0') {
        chess_peer_set_identity_tokens(peer, user, host);
    }
    if (profile_id[0] != '\0') {
        SDL_strlcpy(peer->profile_id, profile_id, sizeof(peer->profile_id));
    }
}

static void avahi_resolver_callback(
    AvahiServiceResolver *r,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiResolverEvent event,
    const char *name,
    const char *type,
    const char *domain,
    const char *host_name,
    const AvahiAddress *address,
    uint16_t port,
    AvahiStringList *txt,
    AvahiLookupResultFlags flags,
    void *userdata)
{
    ChessDiscoveryContext *ctx   = (ChessDiscoveryContext *)userdata;
    ChessAvahiContext     *avahi = (ChessAvahiContext *)ctx->platform;

    (void)interface; (void)protocol; (void)type; (void)domain;
    (void)host_name; (void)flags;

    if (event == AVAHI_RESOLVER_FOUND) {
        if (address && address->proto == AVAHI_PROTO_INET && !avahi->pending_peer_ready) {
            uint32_t resolved_ip = ntohl(address->data.ipv4.address);

            /* Loopback substitution — same logic as DNS-SD backend */
            if ((resolved_ip >> 24) == 127) {
                resolved_ip = ctx->local_ipv4;
            }

            avahi->pending_peer.tcp_ipv4 = resolved_ip;

            /* Extract profile_id from service name (strip :port suffix) */
            {
                const char *colon = strchr(name, ':');
                size_t id_len = colon ? (size_t)(colon - name) : strlen(name);
                if (id_len >= sizeof(avahi->pending_peer.peer.profile_id)) {
                    id_len = sizeof(avahi->pending_peer.peer.profile_id) - 1;
                }
                memcpy(avahi->pending_peer.peer.profile_id, name, id_len);
                avahi->pending_peer.peer.profile_id[id_len] = '\0';
            }
            avahi->pending_peer.tcp_port = port;

            avahi_fill_peer_identity_from_txt(&avahi->pending_peer.peer, txt);
            avahi->pending_peer_ready = true;

            SDL_Log("Avahi: peer ready — profile_id=%s port=%u",
                    avahi->pending_peer.peer.profile_id, (unsigned)port);
        }
    } else {
        SDL_Log("Avahi: failed to resolve service '%s': %s",
                name, avahi_strerror(avahi_client_errno(avahi->client)));
    }

    avahi_service_resolver_free(r);
}

static void avahi_browse_callback(
    AvahiServiceBrowser *b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AvahiLookupResultFlags flags,
    void *userdata)
{
    ChessDiscoveryContext *ctx   = (ChessDiscoveryContext *)userdata;
    ChessAvahiContext     *avahi = (ChessAvahiContext *)ctx->platform;

    (void)b; (void)flags;

    switch (event) {
    case AVAHI_BROWSER_NEW:
        /* Skip our own advertisement */
        if (SDL_strcmp(name, avahi->service_name) == 0) {
            SDL_Log("Avahi: skipping own service");
            break;
        }

        /* Skip if already resolving or queued for this exact service */
        if (avahi->resolving &&
            SDL_strcmp(avahi->resolving_profile_id, name) == 0) {
            break;
        }
        {
            bool already_queued = false;
            for (int i = 0; i < avahi->pending_count; i++) {
                if (SDL_strcmp(avahi->pending_queue[i].name, name) == 0) {
                    already_queued = true;
                    break;
                }
            }
            if (already_queued) {
                break;
            }
        }

        /* If a resolve is in progress or a result is pending, queue for later */
        if (avahi->pending_peer_ready || avahi->resolving) {
            if (avahi->pending_count < CHESS_AVAHI_PENDING_MAX) {
                ChessAvahiPendingService *ps = &avahi->pending_queue[avahi->pending_count++];
                SDL_strlcpy(ps->name, name, sizeof(ps->name));
                SDL_strlcpy(ps->type, type, sizeof(ps->type));
                SDL_strlcpy(ps->domain, domain, sizeof(ps->domain));
                ps->interface = interface;
                ps->protocol  = protocol;
                SDL_Log("Avahi: queued peer service '%s' for later resolve", name);
            }
            break;
        }

        SDL_Log("Avahi: found peer service '%s', resolving...", name);
        memset(&avahi->pending_peer, 0, sizeof(avahi->pending_peer));
        SDL_strlcpy(avahi->resolving_profile_id, name, sizeof(avahi->resolving_profile_id));
        avahi->resolving = true;
        avahi->resolve_started_at = SDL_GetTicks();

        if (!avahi_service_resolver_new(
                avahi->client, interface, protocol,
                name, type, domain,
                AVAHI_PROTO_INET, 0,
                avahi_resolver_callback, ctx)) {
            SDL_Log("Avahi: failed to create resolver: %s",
                    avahi_strerror(avahi_client_errno(avahi->client)));
            avahi->resolving = false;
        }
        break;

    case AVAHI_BROWSER_REMOVE: {
        /* Extract profile_id from service name and signal lobby removal */
        char profile_id[CHESS_PROFILE_ID_STRING_LEN];
        const char *colon = strchr(name, ':');
        size_t uuid_len = colon ? (size_t)(colon - name) : strlen(name);
        if (uuid_len >= sizeof(profile_id)) {
            uuid_len = sizeof(profile_id) - 1;
        }
        memcpy(profile_id, name, uuid_len);
        profile_id[uuid_len] = '\0';

        if (SDL_strcmp(profile_id, ctx->local_peer.profile_id) != 0 &&
            ctx->removal_count < CHESS_DISCOVERY_REMOVAL_QUEUE_MAX) {
            /* Deduplicate: skip if already queued */
            bool already_queued = false;
            for (int q = 0; q < ctx->removal_count; q++) {
                if (SDL_strcmp(ctx->removal_queue[q], profile_id) == 0) {
                    already_queued = true;
                    break;
                }
            }
            if (!already_queued) {
                SDL_strlcpy(ctx->removal_queue[ctx->removal_count],
                            profile_id, CHESS_PROFILE_ID_STRING_LEN);
                ctx->removal_count++;
            }
        }
        SDL_Log("Avahi: service '%s' removed", name);
        break;
    }

    case AVAHI_BROWSER_FAILURE:
        SDL_Log("Avahi: browser failure: %s",
                avahi_strerror(avahi_client_errno(avahi->client)));
        break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        break;
    }
}

static void avahi_group_callback(
    AvahiEntryGroup *g,
    AvahiEntryGroupState state,
    void *userdata)
{
    (void)g; (void)userdata;

    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        SDL_Log("Avahi: service registered");
        break;
    case AVAHI_ENTRY_GROUP_COLLISION:
        SDL_Log("Avahi: service name collision");
        break;
    case AVAHI_ENTRY_GROUP_FAILURE:
        SDL_Log("Avahi: entry group failure");
        break;
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
        break;
    }
}

static void avahi_client_callback(
    AvahiClient *c,
    AvahiClientState state,
    void *userdata)
{
    (void)c; (void)userdata;

    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        SDL_Log("Avahi: client connected to daemon");
        break;
    case AVAHI_CLIENT_FAILURE:
        SDL_Log("Avahi: client failure: %s", avahi_strerror(avahi_client_errno(c)));
        break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
    case AVAHI_CLIENT_CONNECTING:
        break;
    }
}

#endif /* CHESS_APP_HAVE_AVAHI */


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
    {
        ChessAvahiContext *avahi = (ChessAvahiContext *)calloc(1, sizeof(ChessAvahiContext));
        int avahi_error;
        AvahiStringList *txt = NULL;

        if (!avahi) {
            SDL_Log("Avahi: failed to allocate context");
            return false;
        }
        ctx->platform = avahi;

        /* Resolve local IP via getifaddrs */
        {
            uint32_t local_ip = avahi_resolve_local_ip();
            if (local_ip != 0) {
                ctx->local_ipv4 = local_ip;
                SDL_Log("Avahi: local IP resolved to %u.%u.%u.%u",
                        (local_ip >> 24) & 0xffu,
                        (local_ip >> 16) & 0xffu,
                        (local_ip >>  8) & 0xffu,
                         local_ip        & 0xffu);
            } else {
                SDL_Log("Avahi: could not resolve local IP");
            }
        }

        /* Create simple poll object */
        avahi->simple_poll = avahi_simple_poll_new();
        if (!avahi->simple_poll) {
            SDL_Log("Avahi: failed to create simple poll");
            free(avahi);
            ctx->platform = NULL;
            return false;
        }

        /* Create client */
        avahi->client = avahi_client_new(
            avahi_simple_poll_get(avahi->simple_poll),
            0,
            avahi_client_callback,
            ctx,
            &avahi_error);
        if (!avahi->client) {
            SDL_Log("Avahi: failed to create client: %s", avahi_strerror(avahi_error));
            avahi_simple_poll_free(avahi->simple_poll);
            free(avahi);
            ctx->platform = NULL;
            return false;
        }

        /* Register service with TXT records */
        avahi->group = avahi_entry_group_new(
            avahi->client, avahi_group_callback, ctx);
        if (!avahi->group) {
            SDL_Log("Avahi: failed to create entry group: %s",
                    avahi_strerror(avahi_client_errno(avahi->client)));
            avahi_client_free(avahi->client);
            avahi_simple_poll_free(avahi->simple_poll);
            free(avahi);
            ctx->platform = NULL;
            return false;
        }

        txt = avahi_string_list_add_pair(NULL, "user", ctx->local_peer.username);
        txt = avahi_string_list_add_pair(txt, "host", ctx->local_peer.hostname);
        txt = avahi_string_list_add_pair(txt, "profile", ctx->local_peer.profile_id);

        (void)snprintf(avahi->service_name, sizeof(avahi->service_name),
                       "%s:%u", ctx->local_peer.profile_id, (unsigned)ctx->game_port);

        avahi_error = avahi_entry_group_add_service_strlst(
            avahi->group,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_INET,
            0,
            avahi->service_name,
            CHESS_AVAHI_SERVICE_TYPE,
            NULL, NULL,
            ctx->game_port,
            txt);
        avahi_string_list_free(txt);

        if (avahi_error < 0) {
            SDL_Log("Avahi: failed to add service: %s", avahi_strerror(avahi_error));
            avahi_entry_group_free(avahi->group);
            avahi_client_free(avahi->client);
            avahi_simple_poll_free(avahi->simple_poll);
            free(avahi);
            ctx->platform = NULL;
            return false;
        }

        avahi_error = avahi_entry_group_commit(avahi->group);
        if (avahi_error < 0) {
            SDL_Log("Avahi: failed to commit entry group: %s", avahi_strerror(avahi_error));
            avahi_entry_group_free(avahi->group);
            avahi_client_free(avahi->client);
            avahi_simple_poll_free(avahi->simple_poll);
            free(avahi);
            ctx->platform = NULL;
            return false;
        }

        /* Start browsing for peers */
        avahi->browser = avahi_service_browser_new(
            avahi->client,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_INET,
            CHESS_AVAHI_SERVICE_TYPE,
            NULL, 0,
            avahi_browse_callback,
            ctx);
        if (!avahi->browser) {
            SDL_Log("Avahi: failed to create browser: %s",
                    avahi_strerror(avahi_client_errno(avahi->client)));
            avahi_entry_group_free(avahi->group);
            avahi_client_free(avahi->client);
            avahi_simple_poll_free(avahi->simple_poll);
            free(avahi);
            ctx->platform = NULL;
            return false;
        }

        SDL_Log("Avahi: advertising '%s' and browsing for peers on port %u",
                CHESS_AVAHI_SERVICE_TYPE, (unsigned)ctx->game_port);
    }
#elif defined(CHESS_APP_HAVE_DNSSD)
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

#if defined(CHESS_APP_HAVE_AVAHI)
    {
        ChessAvahiContext *avahi = (ChessAvahiContext *)ctx->platform;
        if (avahi) {
            if (avahi->browser)     { avahi_service_browser_free(avahi->browser); }
            if (avahi->group)       { avahi_entry_group_free(avahi->group); }
            if (avahi->client)      { avahi_client_free(avahi->client); }
            if (avahi->simple_poll) { avahi_simple_poll_free(avahi->simple_poll); }
            free(avahi);
        }
    }
#elif defined(CHESS_APP_HAVE_DNSSD)
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
#elif defined(CHESS_APP_HAVE_AVAHI)
    /* Avahi uses its own internal poll; no fds to expose */
    (void)out_fds; (void)max_fds;
    return 0;
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
#elif defined(CHESS_APP_HAVE_AVAHI)
    {
        ChessAvahiContext *avahi = (ChessAvahiContext *)ctx->platform;
        (void)readable; (void)fd_count;
        if (avahi) {
            avahi_simple_poll_iterate(avahi->simple_poll, 0);
        }
    }
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
    {
        ChessAvahiContext *avahi = (ChessAvahiContext *)ctx->platform;
        if (!avahi) {
            return false;
        }

        /* Detect stalled resolve */
        if (!avahi->pending_peer_ready &&
            avahi->resolving &&
            SDL_GetTicks() - avahi->resolve_started_at > CHESS_AVAHI_RESOLVE_TIMEOUT_MS) {
            SDL_Log("Avahi: resolve timed out for '%s', skipping", avahi->resolving_profile_id);
            avahi->resolving = false;
            avahi->resolving_profile_id[0] = '\0';
            memset(&avahi->pending_peer, 0, sizeof(avahi->pending_peer));
        }

        /* Advance: either a peer was resolved, or pipeline was cleared */
        if (avahi->pending_peer_ready ||
            (!avahi->resolving && avahi->pending_count > 0)) {

            bool has_result = avahi->pending_peer_ready;
            if (has_result) {
                *out_remote_peer = avahi->pending_peer;
                avahi->pending_peer_ready = false;
                avahi->resolving = false;
                memset(&avahi->pending_peer, 0, sizeof(avahi->pending_peer));
                avahi->resolving_profile_id[0] = '\0';
            }

            /* Start resolving the next queued service, if any */
            while (avahi->pending_count > 0) {
                ChessAvahiPendingService ps = avahi->pending_queue[0];
                avahi->pending_count--;
                if (avahi->pending_count > 0) {
                    memmove(&avahi->pending_queue[0], &avahi->pending_queue[1],
                            (size_t)avahi->pending_count * sizeof(avahi->pending_queue[0]));
                }

                SDL_Log("Avahi: found peer service '%s', resolving...", ps.name);
                memset(&avahi->pending_peer, 0, sizeof(avahi->pending_peer));
                SDL_strlcpy(avahi->resolving_profile_id, ps.name,
                            sizeof(avahi->resolving_profile_id));
                avahi->resolving = true;
                avahi->resolve_started_at = SDL_GetTicks();

                if (avahi_service_resolver_new(
                        avahi->client, ps.interface, ps.protocol,
                        ps.name, ps.type, ps.domain,
                        AVAHI_PROTO_INET, 0,
                        avahi_resolver_callback, ctx)) {
                    break;
                }
                SDL_Log("Avahi: failed to create resolver: %s",
                        avahi_strerror(avahi_client_errno(avahi->client)));
                avahi->resolving = false;
                avahi->resolving_profile_id[0] = '\0';
            }

            return has_result;
        }

        return false;
    }
#elif defined(CHESS_APP_HAVE_DNSSD)
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
