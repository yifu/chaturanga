/**
 * Handshake-phase packet handlers: HELLO, OFFER, ACCEPT.
 *
 * Split from packet_handlers.c for focused module size.
 */
#include "packet_handlers_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <string.h>

/* ── HELLO ──────────────────────────────────────────────────────────── */

void chess_pkt_handle_hello(AppContext *ctx, const ChessHelloPayload *hello)
{
    if (!ctx || !hello) {
        return;
    }

    ctx->network.network_session.hello_received = true;

    /* If we already know this peer from mDNS, verify that the HELLO
     * identity matches what the discovery layer advertised. */
    if (ctx->network.network_session.peer_available &&
        ctx->network.network_session.remote_peer.username[0] != '\0') {
        if (hello->username[0] != '\0' &&
            SDL_strncmp(ctx->network.network_session.remote_peer.username,
                        hello->username,
                        sizeof(ctx->network.network_session.remote_peer.username)) != 0) {
            SDL_Log("NET: HELLO identity mismatch: expected user '%s', got '%s'",
                    ctx->network.network_session.remote_peer.username, hello->username);
        }
        if (hello->hostname[0] != '\0' &&
            SDL_strncmp(ctx->network.network_session.remote_peer.hostname,
                        hello->hostname,
                        sizeof(ctx->network.network_session.remote_peer.hostname)) != 0) {
            SDL_Log("NET: HELLO identity mismatch: expected host '%s', got '%s'",
                    ctx->network.network_session.remote_peer.hostname, hello->hostname);
        }
    }

    /* When the connection was accepted before mDNS discovery,
     * register minimal identity so that the later set_remote()
     * from mDNS sees same_remote == true and does not reset. */
    if (!ctx->network.network_session.peer_available && hello->profile_id[0] != '\0') {
        memset(&ctx->network.network_session.remote_peer, 0, sizeof(ctx->network.network_session.remote_peer));
        SDL_strlcpy(ctx->network.network_session.remote_peer.profile_id, hello->profile_id,
                     sizeof(ctx->network.network_session.remote_peer.profile_id));
        SDL_strlcpy(ctx->network.network_session.remote_peer.username, hello->username,
                     sizeof(ctx->network.network_session.remote_peer.username));
        SDL_strlcpy(ctx->network.network_session.remote_peer.hostname, hello->hostname,
                     sizeof(ctx->network.network_session.remote_peer.hostname));
        ctx->network.network_session.peer_available = true;
    }

    SDL_Log("NET: received HELLO from remote peer (%.8s...)", hello->profile_id);
}

/* ── OFFER ──────────────────────────────────────────────────────────── */

void chess_pkt_handle_offer(AppContext *ctx, const ChessOfferPayload *offer)
{
    int peer_idx = -1;
    int i;

    if (!ctx || !offer || ctx->network.network_session.challenge_done) {
        return;
    }

    SDL_Log("NET: received OFFER from remote peer (%.8s...)", offer->challenger_profile_id);

    for (i = 0; i < ctx->game.lobby.discovered_peer_count; ++i) {
        if (SDL_strncmp(ctx->game.lobby.discovered_peers[i].peer.profile_id, offer->challenger_profile_id, CHESS_PROFILE_ID_STRING_LEN) == 0) {
            peer_idx = i;
            break;
        }
    }

    if (peer_idx >= 0) {
        /* If we already sent an OFFER to this peer (cross-offer),
         * auto-accept instead of overwriting with INCOMING_PENDING.
         * Use the transport (the server-side connection) for the game. */
        if (chess_lobby_get_challenge_state(&ctx->game.lobby, peer_idx) == CHESS_CHALLENGE_OUTGOING_PENDING) {
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(accept.acceptor_profile_id));
            if (transport_send_accept(&ctx->network.transport.base, &accept)) {
                /* Close all outgoing challenge connections */
                chess_lobby_close_all_challenge_connections(&ctx->game.lobby);
                /* Clear other outgoing challenges */
                {
                    int j;
                    for (j = 0; j < ctx->game.lobby.discovered_peer_count; ++j) {
                        if (j != peer_idx && ctx->game.lobby.discovered_peers[j].challenge_state == CHESS_CHALLENGE_OUTGOING_PENDING) {
                            chess_lobby_set_challenge_state(&ctx->game.lobby, j, CHESS_CHALLENGE_NONE);
                        }
                    }
                }
                ctx->network.network_session.challenge_done = true;
                ctx->network.network_session.role = CHESS_ROLE_SERVER;
                ctx->network.network_session.hello_completed = true;
                chess_lobby_set_challenge_state(&ctx->game.lobby, peer_idx, CHESS_CHALLENGE_MATCHED);
                chess_network_session_set_remote(&ctx->network.network_session, &ctx->game.lobby.discovered_peers[peer_idx].peer);
                chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_GAME_STARTING);
                SDL_Log("NET: cross-offer detected, auto-accepted (%.8s...) role=SERVER",
                        offer->challenger_profile_id);
            } else {
                chess_lobby_set_challenge_state(&ctx->game.lobby, peer_idx, CHESS_CHALLENGE_INCOMING_PENDING);
            }
        } else {
            chess_lobby_set_challenge_state(&ctx->game.lobby, peer_idx, CHESS_CHALLENGE_INCOMING_PENDING);
        }
        chess_network_session_set_remote(&ctx->network.network_session, &ctx->game.lobby.discovered_peers[peer_idx].peer);
    } else {
        /* Peer not yet in lobby (mDNS slower than TCP); buffer the offer
         * so app_poll_discovery_and_update_lobby() can apply it later. */
        ctx->network.network_session.pending_incoming_offer = true;
        SDL_strlcpy(ctx->network.network_session.pending_offer_profile_id,
                     offer->challenger_profile_id,
                     sizeof(ctx->network.network_session.pending_offer_profile_id));
        SDL_Log("NET: OFFER from %.8s... buffered (peer not yet in lobby)",
                offer->challenger_profile_id);
    }
}

/* ── ACCEPT ─────────────────────────────────────────────────────────── */

void chess_pkt_handle_accept(AppContext *ctx, const ChessAcceptPayload *accept)
{
    int peer_idx = -1;
    int i;

    if (!ctx || !accept || ctx->network.network_session.challenge_done) {
        return;
    }

    /* B3 fix: lookup by acceptor profile_id first, fall back to selected_peer_idx. */
    for (i = 0; i < ctx->game.lobby.discovered_peer_count; ++i) {
        if (SDL_strncmp(
                ctx->game.lobby.discovered_peers[i].peer.profile_id,
                accept->acceptor_profile_id,
                CHESS_PROFILE_ID_STRING_LEN) == 0) {
            peer_idx = i;
            break;
        }
    }
    if (peer_idx < 0) {
        peer_idx = ctx->game.lobby.selected_peer_idx;
    }

    ctx->network.network_session.challenge_done = true;
    if (peer_idx >= 0) {
        chess_lobby_set_challenge_state(&ctx->game.lobby, peer_idx, CHESS_CHALLENGE_MATCHED);
    }
    chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_GAME_STARTING);

    SDL_Log("NET: received ACCEPT from remote peer (%.8s...)", accept->acceptor_profile_id);
    SDL_Log("NET: challenge exchange completed (remote accept), waiting START/ACK");
}
