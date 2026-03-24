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
    fd_set rfds;
    struct timeval tv = {0, 5000};

    fd = DNSServiceRefSockFD(ref);
    if (fd < 0) {
        return false;
    }

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
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
        return; /* service removal — ignore */
    }

    /* Skip our own advertisement — use exact match so a Bonjour-renamed
     * duplicate like "<uuid> (2)" is not mistakenly filtered. */
    if (SDL_strcmp(service_name, ctx->local_peer.uuid) == 0) {
        SDL_Log("DNS-SD: skipping own service");
        return;
    }

    /* Skip if already in a resolve/addr pipeline or peer already found */
    if (dnssd->resolve_ref || dnssd->pending_peer_ready) {
        SDL_Log("DNS-SD: already resolving or peer found, skipping '%s'", service_name);
        return;
    }

    SDL_Log("DNS-SD: found peer service '%s', resolving...", service_name);
    memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
    SDL_strlcpy(dnssd->pending_peer.peer.uuid, service_name,
                sizeof(dnssd->pending_peer.peer.uuid));
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

#define CHESS_AVAHI_SERVICE_TYPE "_chess._tcp"

typedef struct {
    AvahiSimplePoll     *simple_poll;
    AvahiClient         *client;
    AvahiEntryGroup     *group;
    AvahiServiceBrowser *browser;
    bool                 pending_peer_ready;
    bool                 resolving;          /* resolver in flight */
    ChessDiscoveredPeer  pending_peer;
    char                 resolving_uuid[CHESS_UUID_STRING_LEN];
} ChessAvahiContext;

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
                resolved_ip = ctx->local_peer.ipv4_host_order;
            }

            avahi->pending_peer.peer.ipv4_host_order = resolved_ip;
            SDL_strlcpy(avahi->pending_peer.peer.uuid, name,
                        sizeof(avahi->pending_peer.peer.uuid));
            avahi->pending_peer.tcp_port = port;

            avahi_fill_peer_identity_from_txt(&avahi->pending_peer.peer, txt);
            avahi->pending_peer_ready = true;

            SDL_Log("Avahi: peer ready — uuid=%s port=%u",
                    name, (unsigned)port);
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
        if (SDL_strcmp(name, ctx->local_peer.uuid) == 0) {
            SDL_Log("Avahi: skipping own service");
            break;
        }

        /* Skip if already resolved/resolving this peer or a result is pending */
        if (avahi->pending_peer_ready) {
            break;
        }
        if (avahi->resolving &&
            SDL_strncmp(avahi->resolving_uuid, name, CHESS_UUID_STRING_LEN) == 0) {
            break;
        }

        SDL_Log("Avahi: found peer service '%s', resolving...", name);
        memset(&avahi->pending_peer, 0, sizeof(avahi->pending_peer));
        SDL_strlcpy(avahi->resolving_uuid, name, sizeof(avahi->resolving_uuid));
        avahi->resolving = true;

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

    case AVAHI_BROWSER_REMOVE:
        SDL_Log("Avahi: service '%s' removed", name);
        break;

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
                ctx->local_peer.ipv4_host_order = local_ip;
                local_peer->ipv4_host_order = local_ip;
                SDL_Log("Avahi: local IP resolved to %u.%u.%u.%u",
                        (local_ip >> 24) & 0xffu,
                        (local_ip >> 16) & 0xffu,
                        (local_ip >>  8) & 0xffu,
                         local_ip        & 0xffu);
            } else {
                SDL_Log("Avahi: could not resolve local IP, election will use UUID only");
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

        avahi_error = avahi_entry_group_add_service_strlst(
            avahi->group,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_INET,
            0,
            ctx->local_peer.uuid,
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
            ctx->local_peer.uuid,
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
    if (!ctx || !ctx->started || !out_remote_peer) {
        return false;
    }

#if defined(CHESS_APP_HAVE_AVAHI)
    {
        ChessAvahiContext *avahi = (ChessAvahiContext *)ctx->platform;
        if (!avahi) {
            return false;
        }

        /* Pump Avahi event loop (non-blocking) */
        avahi_simple_poll_iterate(avahi->simple_poll, 0);

        if (avahi->pending_peer_ready) {
            *out_remote_peer = avahi->pending_peer;
            avahi->pending_peer_ready = false;
            memset(&avahi->pending_peer, 0, sizeof(avahi->pending_peer));
            return true;
        }

        return false;
    }
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
            dnssd->pending_peer_ready = false;
            memset(&dnssd->pending_peer, 0, sizeof(dnssd->pending_peer));
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
            return false;
        }

        errno = 0;
        parsed_port = strtol(port_str, &endptr, 10);
        if (errno != 0 || endptr == port_str || *endptr != '\0' || parsed_port <= 0 || parsed_port > 65535) {
            SDL_Log("Ignoring CHESS_REMOTE_PORT: invalid port '%s'", port_str);
            return false;
        }

        SDL_strlcpy(out_remote_peer->peer.uuid, uuid, sizeof(out_remote_peer->peer.uuid));
        out_remote_peer->tcp_port = (uint16_t)parsed_port;

        if (SDL_strncmp(out_remote_peer->peer.uuid, ctx->local_peer.uuid, CHESS_UUID_STRING_LEN) == 0) {
            SDL_Log("Ignoring discovered peer because UUID matches local peer");
            return false;
        }

        return true;
    }
#endif
}
