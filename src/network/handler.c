#include "chess_app/net_handler.h"
#include "handler_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/persistence.h"
#include "chess_app/ui_game.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_protocol.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/tcp_transport.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>

static uint32_t make_game_id(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer)
{
    uint32_t hash = 2166136261u;
    const char *ids[2] = { NULL, NULL };
    int i = 0;
    int j = 0;

    if (!local_peer || !remote_peer) {
        return 0u;
    }

    ids[0] = (SDL_strncmp(local_peer->profile_id, remote_peer->profile_id, CHESS_PROFILE_ID_STRING_LEN) <= 0)
        ? local_peer->profile_id
        : remote_peer->profile_id;
    ids[1] = (ids[0] == local_peer->profile_id) ? remote_peer->profile_id : local_peer->profile_id;

    for (i = 0; i < 2; ++i) {
        for (j = 0; ids[i][j] != '\0'; ++j) {
            hash ^= (uint8_t)ids[i][j];
            hash *= 16777619u;
        }
    }

    return hash;
}

/* ── Socket polling ─────────────────────────────────────────────────── */

/* Central poll: listener + game connection + challenge fds + discovery fds */
static void poll_socket_events(
    const ChessTcpListener *listener,
    int conn_fd,
    const ChessLobbyState *lobby,
    ChessDiscoveryContext *discovery,
    ChessNetPollResult *events)
{
    struct pollfd fds[2 + CHESS_MAX_DISCOVERED_PEERS + CHESS_DISCOVERY_MAX_POLL_FDS];
    int nfds = 0;
    int listener_idx = -1;
    int connection_idx = -1;
    int challenge_fd_idx[CHESS_MAX_DISCOVERED_PEERS];
    int discovery_fds[CHESS_DISCOVERY_MAX_POLL_FDS];
    int discovery_base_idx = -1;
    int i;
    int ret;

    if (!events) {
        return;
    }

    memset(events, 0, sizeof(*events));
    for (i = 0; i < CHESS_MAX_DISCOVERED_PEERS; ++i) {
        challenge_fd_idx[i] = -1;
    }

    if (listener && listener->fd >= 0) {
        listener_idx = nfds;
        fds[nfds].fd = listener->fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    if (conn_fd >= 0) {
        connection_idx = nfds;
        fds[nfds].fd = conn_fd;
        fds[nfds].events = POLLIN | POLLOUT;
        fds[nfds].revents = 0;
        nfds++;
    }

    if (lobby) {
        for (i = 0; i < lobby->discovered_peer_count; ++i) {
            int cfd = lobby->discovered_peers[i].challenge_conn.fd;
            if (cfd >= 0) {
                challenge_fd_idx[i] = nfds;
                fds[nfds].fd = cfd;
                fds[nfds].events = lobby->discovered_peers[i].challenge_conn.connect_in_progress
                    ? POLLOUT
                    : POLLIN;
                fds[nfds].revents = 0;
                nfds++;
            }
        }
    }

    /* Discovery fds (DNS-SD refs) */
    events->discovery_fd_count = chess_discovery_get_poll_fds(discovery, discovery_fds, CHESS_DISCOVERY_MAX_POLL_FDS);
    if (events->discovery_fd_count > 0) {
        discovery_base_idx = nfds;
        for (i = 0; i < events->discovery_fd_count; ++i) {
            fds[nfds].fd = discovery_fds[i];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
    }

    if (nfds == 0) {
        return;
    }

    ret = poll(fds, (nfds_t)nfds, 0);
    if (ret <= 0) {
        return;
    }

    if (listener_idx >= 0 && (fds[listener_idx].revents & POLLIN)) {
        events->listener_readable = true;
    }
    if (connection_idx >= 0) {
        events->connection_readable = (fds[connection_idx].revents & POLLIN) != 0;
        events->connection_writable = (fds[connection_idx].revents & POLLOUT) != 0;
    }
    if (lobby) {
        for (i = 0; i < lobby->discovered_peer_count; ++i) {
            if (challenge_fd_idx[i] >= 0) {
                if (fds[challenge_fd_idx[i]].revents & POLLIN) {
                    events->challenge_readable[i] = true;
                }
                if (fds[challenge_fd_idx[i]].revents & POLLOUT) {
                    events->challenge_writable[i] = true;
                }
            }
        }
    }
    if (discovery_base_idx >= 0) {
        for (i = 0; i < events->discovery_fd_count; ++i) {
            events->discovery_readable[i] = (fds[discovery_base_idx + i].revents & POLLIN) != 0;
        }
    }
}

/* ── Transport progress ─────────────────────────────────────────────── */

void chess_net_reset_transport_progress(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->network.network_session.connect_attempted = false;
    ctx->network.network_session.hello_sent = false;
    ctx->network.network_session.hello_received = false;
    ctx->network.network_session.hello_completed = false;
    ctx->network.network_session.challenge_done = false;
    ctx->network.network_session.start_sent = false;
    ctx->network.network_session.start_sent_at_ms = 0;
    ctx->network.network_session.resume_request_sent = false;
    ctx->network.network_session.pending_resume_state_sync = false;
}

/* ── Incoming packet processing (defined in packet_handlers.c) ─────── */

void chess_net_drain_incoming_packets(AppContext *ctx, bool readable);

/* ── Outgoing send helpers ──────────────────────────────────────────── */

static void net_send_state_snapshot_if_needed(AppContext *ctx)
{
    ChessStateSnapshotPayload snapshot;
    Transport *t;

    if (!ctx ||
        ctx->network.network_session.role != CHESS_ROLE_SERVER ||
        !ctx->network.network_session.pending_resume_state_sync ||
        !ctx->network.network_session.start_completed) {
        return;
    }

    t = &ctx->network.transport.base;
    if (transport_get_fd(t) < 0) {
        return;
    }

    if (!chess_persist_build_state_snapshot_payload(ctx, &snapshot)) {
        return;
    }

    if (transport_send_state_snapshot(t, &snapshot)) {
        ctx->network.network_session.pending_resume_state_sync = false;
        SDL_Log("NET: sent state snapshot for resumed game %u", snapshot.game_id);
    }
}

static void net_advance_transport_connection(AppContext *ctx, const ChessNetPollResult *socket_events)
{
    Transport *t;

    if (!ctx || !socket_events) {
        return;
    }

    if (ctx->network.network_session.hello_completed) {
        return;
    }

    /* Don't accept connections or initiate handshakes in post-game phases;
     * the user must click "Return to Lobby" first. */
    if (ctx->network.network_session.phase == CHESS_PHASE_DISCONNECTED ||
        ctx->network.network_session.phase == CHESS_PHASE_TERMINATED) {
        return;
    }

    t = &ctx->network.transport.base;

    /* CLIENT-side: initiate outgoing TCP connect for resume reconnection.
     * The resume flow sets role=CLIENT and phase=TCP_CONNECTING when the
     * persisted resume peer is rediscovered via mDNS. */
    if (transport_get_fd(t) < 0 &&
        ctx->network.network_session.role == CHESS_ROLE_CLIENT &&
        ctx->network.network_session.phase == CHESS_PHASE_TCP_CONNECTING) {
        const uint64_t now = SDL_GetTicks();
        int peer_idx = -1;
        int i;

        if (ctx->network.next_connect_attempt_at != 0 && now < ctx->network.next_connect_attempt_at) {
            goto accept_inbound;
        }

        for (i = 0; i < ctx->game.lobby.discovered_peer_count; ++i) {
            if (SDL_strncmp(ctx->game.lobby.discovered_peers[i].peer.profile_id,
                            ctx->network.network_session.remote_peer.profile_id,
                            CHESS_PROFILE_ID_STRING_LEN) == 0) {
                peer_idx = i;
                break;
            }
        }

        if (peer_idx >= 0) {
            int new_fd = -1;
            ChessDiscoveredPeerState *ps = &ctx->game.lobby.discovered_peers[peer_idx];

            if (chess_tcp_connect_start(ps->tcp_ipv4, ps->tcp_port, &new_fd)) {
                tcp_transport_init_from_fd(&ctx->network.transport, new_fd);
                /* Check if already connected (immediate local connect) */
                {
                    ChessConnectResult cr = chess_tcp_connect_check(new_fd);
                    if (cr == CHESS_CONNECT_CONNECTED) {
                        transport_set_nonblocking(t);
                        transport_recv_reset(t);
                        ctx->network.network_session.transport_connected = true;
                        ctx->network.next_connect_attempt_at = 0;
                        SDL_Log("NET: connected to resume peer %d (%.8s...)", peer_idx, ps->peer.profile_id);
                    }
                    /* else: IN_PROGRESS — connection_writable from central poll will finalize */
                }
            } else {
                ctx->network.next_connect_attempt_at = now + (uint64_t)ctx->network.connect_retry_ms;
            }
        }
    }

    /* Finalize async resume connect when central poll reports writable */
    if (transport_get_fd(t) >= 0 &&
        ctx->network.network_session.role == CHESS_ROLE_CLIENT &&
        ctx->network.network_session.phase == CHESS_PHASE_TCP_CONNECTING &&
        !ctx->network.network_session.transport_connected &&
        socket_events->connection_writable) {
        ChessConnectResult cr = chess_tcp_connect_check(transport_get_fd(t));
        if (cr == CHESS_CONNECT_CONNECTED) {
            transport_set_nonblocking(t);
            transport_recv_reset(t);
            ctx->network.network_session.transport_connected = true;
            ctx->network.next_connect_attempt_at = 0;
            SDL_Log("NET: resume connect completed");
        } else if (cr == CHESS_CONNECT_FAILED) {
            SDL_Log("NET: resume connect failed, will retry");
            transport_close(t);
            ctx->network.next_connect_attempt_at = SDL_GetTicks() + (uint64_t)ctx->network.connect_retry_ms;
        }
    }

accept_inbound:
    /* Accept inbound TCP connections eagerly — even before mDNS
     * resolves the remote peer.  The HELLO handshake identifies
     * the peer.  This eliminates ~800 ms of Bonjour propagation
     * delay on reconnect. */
    if (transport_get_fd(t) < 0 &&
        ctx->network.listener.fd >= 0 &&
        socket_events->listener_readable) {
        ChessTcpConnection accepted_conn;
        accepted_conn.fd = -1;
        if (chess_tcp_accept_once(&ctx->network.listener, 0, &accepted_conn)) {
            tcp_transport_init_from_fd(&ctx->network.transport, accepted_conn.fd);
            transport_set_nonblocking(t);
            SDL_Log("NET: accepted TCP client connection");
        }
    }
}

static void net_advance_hello_handshake(AppContext *ctx)
{
    ChessRole effective_role;
    Transport *t;

    if (!ctx) {
        return;
    }

    t = &ctx->network.transport.base;
    if (transport_get_fd(t) < 0) {
        return;
    }

    if (ctx->network.network_session.hello_completed) {
        return;
    }

    /* Infer server role when we accepted an inbound connection
     * before the mDNS election completed (role still UNKNOWN).
     * Client-side HELLO on the transport is used for resume reconnection;
     * challenge HELLO is handled per-peer in chess_net_advance_outgoing_challenges. */
    effective_role = ctx->network.network_session.role;
    if (effective_role == CHESS_ROLE_UNKNOWN) {
        effective_role = CHESS_ROLE_SERVER;
    }

    /* CLIENT-side HELLO (resume reconnection).
     * Wait until async connect is finalized (transport_connected). */
    if (effective_role == CHESS_ROLE_CLIENT) {
        if (!ctx->network.network_session.transport_connected) {
            return;
        }

        if (!ctx->network.network_session.hello_sent) {
            ChessHelloPayload local_hello;
            memset(&local_hello, 0, sizeof(local_hello));
            SDL_strlcpy(local_hello.profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(local_hello.profile_id));
            SDL_strlcpy(local_hello.username, ctx->network.network_session.local_peer.username, sizeof(local_hello.username));
            SDL_strlcpy(local_hello.hostname, ctx->network.network_session.local_peer.hostname, sizeof(local_hello.hostname));
            local_hello.role = (uint32_t)CHESS_ROLE_CLIENT;
            if (transport_send_hello(t, &local_hello)) {
                ctx->network.network_session.hello_sent = true;
                SDL_Log("NET: sent HELLO (client, resume)");
            }
        }

        if (ctx->network.network_session.hello_sent && ctx->network.network_session.hello_received) {
            ctx->network.network_session.hello_completed = true;
            chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_AUTHENTICATED);
            SDL_Log("NET: HELLO handshake completed (client, resume)");
        }
        return;
    }

    /* SERVER-side HELLO */
    if (ctx->network.network_session.role == CHESS_ROLE_UNKNOWN) {
        ctx->network.network_session.role = CHESS_ROLE_SERVER;
        SDL_Log("NET: inferred role SERVER from early accept");
    }

    {
        ChessHelloPayload local_hello;
        memset(&local_hello, 0, sizeof(local_hello));
        SDL_strlcpy(local_hello.profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(local_hello.profile_id));
        SDL_strlcpy(local_hello.username, ctx->network.network_session.local_peer.username, sizeof(local_hello.username));
        SDL_strlcpy(local_hello.hostname, ctx->network.network_session.local_peer.hostname, sizeof(local_hello.hostname));
        local_hello.role = (uint32_t)CHESS_ROLE_SERVER;

        if (ctx->network.network_session.hello_received && !ctx->network.network_session.hello_sent) {
            if (transport_send_hello(t, &local_hello)) {
                ctx->network.network_session.hello_sent = true;
            }
        }

        if (ctx->network.network_session.hello_sent && ctx->network.network_session.hello_received) {
            ctx->network.network_session.hello_completed = true;
            chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_AUTHENTICATED);
            SDL_Log("NET: HELLO handshake completed (server)");
        }
    }
}

/* Outgoing offers are now sent per-peer in chess_net_advance_outgoing_challenges */

static void net_send_start_if_needed(AppContext *ctx)
{
    Transport *t;

    if (!ctx) {
        return;
    }

    t = &ctx->network.transport.base;
    if (!ctx->network.network_session.hello_completed ||
        !ctx->network.network_session.challenge_done ||
        ctx->network.network_session.start_completed ||
        ctx->network.network_session.role != CHESS_ROLE_SERVER ||
        transport_get_fd(t) < 0 ||
        ctx->network.network_session.start_sent) {
        return;
    }

    if (ctx->protocol.pending_start_payload.game_id == 0u) {
        memset(&ctx->protocol.pending_start_payload, 0, sizeof(ctx->protocol.pending_start_payload));
        ctx->protocol.pending_start_payload.game_id = make_game_id(&ctx->network.network_session.local_peer, &ctx->network.network_session.remote_peer);
        ctx->protocol.pending_start_payload.initial_turn = CHESS_COLOR_WHITE;
        if (ctx->protocol.pending_start_payload.resume_token[0] == '\0') {
            (void)chess_generate_peer_uuid(
                ctx->protocol.pending_start_payload.resume_token,
                sizeof(ctx->protocol.pending_start_payload.resume_token));
        }

        {
            const bool server_is_white = (arc4random() % 2u) == 0u;
            if (server_is_white) {
                ctx->protocol.pending_start_payload.assigned_color = CHESS_COLOR_BLACK;
                SDL_strlcpy(ctx->protocol.pending_start_payload.white_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(ctx->protocol.pending_start_payload.white_profile_id));
                SDL_strlcpy(ctx->protocol.pending_start_payload.black_profile_id, ctx->network.network_session.remote_peer.profile_id, sizeof(ctx->protocol.pending_start_payload.black_profile_id));
            } else {
                ctx->protocol.pending_start_payload.assigned_color = CHESS_COLOR_WHITE;
                SDL_strlcpy(ctx->protocol.pending_start_payload.white_profile_id, ctx->network.network_session.remote_peer.profile_id, sizeof(ctx->protocol.pending_start_payload.white_profile_id));
                SDL_strlcpy(ctx->protocol.pending_start_payload.black_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(ctx->protocol.pending_start_payload.black_profile_id));
            }
            SDL_Log("NET: color assignment - server=%s, client=%s",
                server_is_white ? "WHITE" : "BLACK",
                server_is_white ? "BLACK" : "WHITE");
        }
    }

    if (transport_send_start(t, &ctx->protocol.pending_start_payload)) {
        ctx->network.network_session.start_sent = true;
        ctx->network.network_session.start_sent_at_ms = SDL_GetTicks();
    } else {
        ctx->network.network_session.start_failures += 1u;
        if (ctx->network.network_session.start_failures == 1u || (ctx->network.network_session.start_failures % 5u) == 0u) {
            SDL_Log("NET: START send failed (%u failures), will retry", ctx->network.network_session.start_failures);
        }
    }
}

static void net_send_resume_request_if_needed(AppContext *ctx)
{
    ChessResumeRequestPayload request;
    Transport *t;

    if (!ctx ||
        ctx->network.network_session.role != CHESS_ROLE_CLIENT ||
        !ctx->network.network_session.hello_completed ||
        ctx->network.network_session.resume_request_sent ||
        ctx->network.network_session.start_completed ||
        ctx->protocol.pending_start_payload.game_id == 0u ||
        ctx->protocol.pending_start_payload.resume_token[0] == '\0') {
        return;
    }

    t = &ctx->network.transport.base;
    if (transport_get_fd(t) < 0) {
        return;
    }

    memset(&request, 0, sizeof(request));
    request.game_id = ctx->protocol.pending_start_payload.game_id;
    SDL_strlcpy(request.profile_id, ctx->network.local_peer.profile_id, sizeof(request.profile_id));
    SDL_strlcpy(request.resume_token, ctx->protocol.pending_start_payload.resume_token, sizeof(request.resume_token));

    if (transport_send_resume_request(t, &request)) {
        ctx->network.network_session.resume_request_sent = true;
        SDL_Log("NET: sent resume request for game %u", request.game_id);
    }
}

/* ── Public: network tick ───────────────────────────────────────────── */

void chess_net_tick(AppContext *ctx)
{
    ChessNetPollResult poll_result;

    if (!ctx) {
        return;
    }

    /* B1 fix: retry START after 3 seconds if ACK never arrived. */
    if (ctx->network.network_session.start_sent && !ctx->network.network_session.start_completed &&
        (SDL_GetTicks() - ctx->network.network_session.start_sent_at_ms) > 3000u) {
        SDL_Log("NET: START ACK timeout, will retry");
        ctx->network.network_session.start_sent = false;
    }

    /* Single central poll for all network + discovery fds */
    poll_socket_events(&ctx->network.listener, transport_get_fd(&ctx->network.transport.base), &ctx->game.lobby, &ctx->network.discovery, &poll_result);

    /* Process discovery events (DNS-SD / Avahi) based on poll results */
    chess_discovery_process_events(&ctx->network.discovery, poll_result.discovery_readable, poll_result.discovery_fd_count);

    /* Advance per-peer outgoing challenge connections (CLIENT role) */
    chess_net_advance_outgoing_challenges(ctx, &poll_result);

    /* Server-side: accept inbound connections + HELLO handshake */
    net_advance_transport_connection(ctx, &poll_result);
    net_advance_hello_handshake(ctx);
    net_send_resume_request_if_needed(ctx);

    chess_net_drain_incoming_packets(ctx, poll_result.connection_readable);
    net_send_start_if_needed(ctx);
    net_send_state_snapshot_if_needed(ctx);
}
