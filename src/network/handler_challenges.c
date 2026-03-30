#include "handler_internal.h"

#include "chess_app/lobby_state.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_protocol.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/tcp_transport.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <string.h>
#include <unistd.h>

/* Promote a per-peer outgoing challenge connection to the main game connection.
 * Closes all other challenge connections and transitions to GAME_STARTING. */
static void net_promote_challenge_to_game(AppContext *ctx, int peer_idx)
{
    int i;
    ChessDiscoveredPeerState *ps;
    Transport *t;

    if (!ctx || peer_idx < 0 || peer_idx >= ctx->game.lobby.discovered_peer_count) {
        return;
    }

    ps = &ctx->game.lobby.discovered_peers[peer_idx];
    t = &ctx->network.transport.base;

    /* Take over the challenge fd as the game connection */
    {
        int fd = ps->challenge_conn.fd;
        ps->challenge_conn.fd = -1; /* prevent close_challenge_connection from closing it */
        transport_close(t);
        tcp_transport_init_from_fd(&ctx->network.transport, fd);
        transport_set_nonblocking(t);
    }

    /* Close all other challenge connections */
    for (i = 0; i < ctx->game.lobby.discovered_peer_count; ++i) {
        if (i != peer_idx) {
            chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
            if (ctx->game.lobby.discovered_peers[i].challenge_state == CHESS_CHALLENGE_OUTGOING_PENDING) {
                chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
            }
        }
    }

    /* Set up the game session */
    chess_lobby_set_challenge_state(&ctx->game.lobby, peer_idx, CHESS_CHALLENGE_MATCHED);
    ctx->network.network_session.challenge_done = true;
    ctx->network.network_session.role = CHESS_ROLE_CLIENT;
    ctx->network.network_session.transport_connected = true;
    ctx->network.network_session.hello_sent = true;
    ctx->network.network_session.hello_received = true;
    ctx->network.network_session.hello_completed = true;
    chess_network_session_set_remote(&ctx->network.network_session, &ps->peer);
    chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_GAME_STARTING);

    SDL_Log("NET: challenge exchange completed (remote accept), waiting START/ACK");
}

/* Advance per-peer outgoing challenge connections (connect + HELLO + OFFER).
 * Each peer with OUTGOING_PENDING gets its own TCP fd and handshake state. */
void chess_net_advance_outgoing_challenges(AppContext *ctx, const ChessNetPollResult *poll_result)
{
    int i;
    const uint64_t now = SDL_GetTicks();

    if (!ctx) {
        return;
    }

    /* Don't advance challenges if we're already in a game or game-starting */
    if (ctx->network.network_session.challenge_done ||
        ctx->network.network_session.phase == CHESS_PHASE_GAME_STARTING ||
        ctx->network.network_session.phase == CHESS_PHASE_IN_GAME ||
        ctx->network.network_session.phase == CHESS_PHASE_DISCONNECTED ||
        ctx->network.network_session.phase == CHESS_PHASE_TERMINATED) {
        return;
    }

    for (i = 0; i < ctx->game.lobby.discovered_peer_count; ++i) {
        ChessDiscoveredPeerState *ps = &ctx->game.lobby.discovered_peers[i];
        ChessChallengeConnection *cc = &ps->challenge_conn;

        if (ps->challenge_state != CHESS_CHALLENGE_OUTGOING_PENDING) {
            continue;
        }

        /* ── TCP connect ───────────────────────────────────────────── */
        if (cc->fd < 0) {
            if (!cc->connect_attempted || now >= cc->next_connect_attempt_at) {
                int new_fd = -1;
                cc->connect_attempted = true;
                cc->next_connect_attempt_at = now + (uint64_t)ctx->network.connect_retry_ms;

                if (chess_tcp_connect_start(ps->tcp_ipv4, ps->tcp_port, &new_fd)) {
                    cc->fd = new_fd;
                    cc->connect_in_progress = true;
                    SDL_Log("NET: connect started to peer %d (%.8s...) for challenge", i, ps->peer.profile_id);
                }
            }
            continue;
        }

        /* ── Finalize async connect ────────────────────────────────── */
        if (cc->connect_in_progress) {
            if (poll_result->challenge_writable[i]) {
                ChessConnectResult cr = chess_tcp_connect_check(cc->fd);
                if (cr == CHESS_CONNECT_CONNECTED) {
                    cc->connect_in_progress = false;
                    cc->connect_failures = 0;
                    SDL_Log("NET: connected to peer %d (%.8s...) for challenge", i, ps->peer.profile_id);
                } else if (cr == CHESS_CONNECT_FAILED) {
                    cc->connect_failures++;
                    if (cc->connect_failures >= 3) {
                        SDL_Log("NET: TCP connect to peer %d (%.8s...) failed %u times, marking unreachable",
                                i, ps->peer.profile_id, cc->connect_failures);
                        chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_CONNECT_FAILED);
                    } else {
                        SDL_Log("NET: connect failed to peer %d (%.8s...), will retry", i, ps->peer.profile_id);
                    }
                    close(cc->fd);
                    cc->fd = -1;
                    cc->connect_in_progress = false;
                    cc->connect_attempted = false;
                }
            }
            continue;
        }

        /* ── HELLO handshake ───────────────────────────────────────── */
        if (!cc->hello_completed) {
            /* Client sends HELLO first */
            if (!cc->hello_sent) {
                ChessHelloPayload hello;
                ChessTcpConnection tmp = { .fd = cc->fd };
                memset(&hello, 0, sizeof(hello));
                SDL_strlcpy(hello.profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(hello.profile_id));
                SDL_strlcpy(hello.username, ctx->network.network_session.local_peer.username, sizeof(hello.username));
                SDL_strlcpy(hello.hostname, ctx->network.network_session.local_peer.hostname, sizeof(hello.hostname));
                hello.role = (uint32_t)CHESS_ROLE_CLIENT;
                if (chess_tcp_send_hello(&tmp, &hello)) {
                    cc->hello_sent = true;
                } else {
                    SDL_Log("NET: challenge HELLO send failed for peer %d, closing", i);
                    chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
                    chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
                    continue;
                }
            }

            /* Try to receive HELLO reply (non-blocking) */
            if (cc->hello_sent && !cc->hello_received && poll_result->challenge_readable[i]) {
                ChessPacketHeader hdr;
                ChessHelloPayload remote_hello;
                ChessTcpConnection tmp = { .fd = cc->fd };
                ChessRecvResult rr = chess_tcp_recv_nonblocking(
                    &tmp, &cc->recv_buf, &hdr,
                    (uint8_t *)&remote_hello, sizeof(remote_hello));
                if (rr == CHESS_RECV_OK) {
                    if (hdr.message_type == CHESS_MSG_HELLO && hdr.payload_size == sizeof(ChessHelloPayload)) {
                        /* Verify HELLO identity matches mDNS-discovered peer */
                        if (ps->peer.username[0] != '\0' && remote_hello.username[0] != '\0' &&
                            SDL_strncmp(ps->peer.username, remote_hello.username, sizeof(ps->peer.username)) != 0) {
                            SDL_Log("NET: challenge HELLO identity mismatch for peer %d: "
                                    "expected user '%s', got '%s'",
                                    i, ps->peer.username, remote_hello.username);
                        }
                        if (ps->peer.hostname[0] != '\0' && remote_hello.hostname[0] != '\0' &&
                            SDL_strncmp(ps->peer.hostname, remote_hello.hostname, sizeof(ps->peer.hostname)) != 0) {
                            SDL_Log("NET: challenge HELLO identity mismatch for peer %d: "
                                    "expected host '%s', got '%s'",
                                    i, ps->peer.hostname, remote_hello.hostname);
                        }
                        cc->hello_received = true;
                        SDL_Log("NET: challenge HELLO completed with peer %d (%.8s...)", i, ps->peer.profile_id);
                    } else {
                        SDL_Log("NET: unexpected packet during challenge HELLO for peer %d, closing", i);
                        chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
                        chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
                        continue;
                    }
                } else if (rr == CHESS_RECV_ERROR) {
                    SDL_Log("NET: challenge HELLO recv error for peer %d, closing", i);
                    chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
                    chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
                    continue;
                }
                /* CHESS_RECV_INCOMPLETE: partial data, will continue next frame */
            }

            if (cc->hello_sent && cc->hello_received) {
                cc->hello_completed = true;
            }
            continue;
        }

        /* ── Send OFFER ────────────────────────────────────────────── */
        if (!ps->offer_sent) {
            ChessOfferPayload offer;
            ChessTcpConnection tmp = { .fd = cc->fd };
            memset(&offer, 0, sizeof(offer));
            SDL_strlcpy(offer.challenger_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(offer.challenger_profile_id));
            if (chess_tcp_send_offer(&tmp, &offer)) {
                ps->offer_sent = true;
                SDL_Log("NET: sent OFFER to peer %d (%.8s...)", i, ps->peer.profile_id);
            } else {
                SDL_Log("NET: challenge OFFER send failed for peer %d, closing", i);
                chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
                chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
                continue;
            }
        }

        /* ── Wait for ACCEPT on this challenge connection ──────────── */
        if (ps->offer_sent && poll_result->challenge_readable[i]) {
            ChessPacketHeader hdr;
            uint8_t payload_buf[sizeof(ChessHelloPayload)]; /* largest challenge payload */
            ChessTcpConnection tmp = { .fd = cc->fd };
            ChessRecvResult rr = chess_tcp_recv_nonblocking(
                &tmp, &cc->recv_buf, &hdr,
                payload_buf, sizeof(payload_buf));

            if (rr == CHESS_RECV_OK) {
                if (hdr.message_type == CHESS_MSG_ACCEPT && hdr.payload_size == sizeof(ChessAcceptPayload)) {
                    /* Promote this connection to the game connection */
                    SDL_Log("NET: received ACCEPT from peer %d (%.8s...)", i, ps->peer.profile_id);
                    net_promote_challenge_to_game(ctx, i);
                    return; /* stop iterating, we're in-game now */
                } else if (hdr.message_type == CHESS_MSG_OFFER && hdr.payload_size == sizeof(ChessOfferPayload)) {
                    /* Cross-offer: auto-accept */
                    ChessAcceptPayload accept;
                    memset(&accept, 0, sizeof(accept));
                    SDL_strlcpy(accept.acceptor_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(accept.acceptor_profile_id));
                    if (chess_tcp_send_accept(&tmp, &accept)) {
                        SDL_Log("NET: cross-offer with peer %d (%.8s...), auto-accepted", i, ps->peer.profile_id);
                        net_promote_challenge_to_game(ctx, i);
                        return;
                    } else {
                        SDL_Log("NET: challenge ACCEPT send failed for peer %d, closing", i);
                        chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
                        chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
                        continue;
                    }
                } else {
                    /* Unexpected packet — ignore */
                    SDL_Log("NET: unexpected packet type %u on challenge fd for peer %d", hdr.message_type, i);
                }
            } else if (rr == CHESS_RECV_ERROR) {
                /* Connection lost */
                SDL_Log("NET: challenge connection lost with peer %d (%.8s...)", i, ps->peer.profile_id);
                chess_lobby_close_challenge_connection(&ctx->game.lobby, i);
                chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
            }
            /* CHESS_RECV_INCOMPLETE: partial data, will continue next frame */
        }
    }
}
