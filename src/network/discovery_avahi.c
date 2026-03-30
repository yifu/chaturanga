/* ============================================================
 * Avahi backend — Linux
 *
 * Backend implementation for mDNS peer discovery using Avahi.
 * Compiled to an empty translation unit when CHESS_APP_HAVE_AVAHI
 * is not defined.
 * ============================================================ */

#include "discovery_internal.h"

#if defined(CHESS_APP_HAVE_AVAHI)

#include "chess_app/network_peer.h"

#include <SDL3/SDL.h>
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
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/*  Static helpers                                                     */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Avahi callbacks                                                    */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Backend API                                                        */
/* ------------------------------------------------------------------ */

bool chess_discovery_avahi_start(ChessDiscoveryContext *ctx)
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
    return true;
}

void chess_discovery_avahi_stop(ChessDiscoveryContext *ctx)
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

int chess_discovery_avahi_get_poll_fds(ChessDiscoveryContext *ctx,
                                       int *out_fds, int max_fds)
{
    /* Avahi uses its own internal poll; no fds to expose */
    (void)ctx; (void)out_fds; (void)max_fds;
    return 0;
}

void chess_discovery_avahi_process_events(ChessDiscoveryContext *ctx,
                                          const bool *readable, int fd_count)
{
    ChessAvahiContext *avahi = (ChessAvahiContext *)ctx->platform;
    (void)readable; (void)fd_count;
    if (avahi) {
        avahi_simple_poll_iterate(avahi->simple_poll, 0);
    }
}

bool chess_discovery_avahi_check_result(ChessDiscoveryContext *ctx,
                                        ChessDiscoveredPeer *out_remote_peer)
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

#endif /* CHESS_APP_HAVE_AVAHI */
