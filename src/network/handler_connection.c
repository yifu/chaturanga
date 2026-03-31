#include "chess_app/net_handler.h"
#include "handler_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/tcp_transport.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <string.h>

void chess_net_advance_transport_connection(AppContext *ctx, const ChessNetPollResult *socket_events)
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

void chess_net_advance_hello_handshake(AppContext *ctx)
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
