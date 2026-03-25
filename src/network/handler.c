#include "chess_app/net_handler.h"

#include "chess_app/app_context.h"
#include "chess_app/persistence.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_protocol.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>

/* ── Utilities ──────────────────────────────────────────────────────── */

static ChessPlayerColor opposite_color(ChessPlayerColor color)
{
    if (color == CHESS_COLOR_WHITE) {
        return CHESS_COLOR_BLACK;
    }
    if (color == CHESS_COLOR_BLACK) {
        return CHESS_COLOR_WHITE;
    }
    return CHESS_COLOR_UNASSIGNED;
}

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

typedef struct ChessSocketEvents {
    bool listener_readable;
    bool connection_readable;
    bool connection_writable;
} ChessSocketEvents;

static void poll_socket_events(
    const ChessTcpListener *listener,
    const ChessTcpConnection *connection,
    ChessSocketEvents *events)
{
    struct pollfd fds[2];
    int nfds = 0;
    int listener_idx = -1;
    int connection_idx = -1;
    int ret;

    if (!events) {
        return;
    }

    memset(events, 0, sizeof(*events));

    if (listener && listener->fd >= 0) {
        listener_idx = nfds;
        fds[nfds].fd = listener->fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    if (connection && connection->fd >= 0) {
        connection_idx = nfds;
        fds[nfds].fd = connection->fd;
        fds[nfds].events = POLLIN | POLLOUT;
        fds[nfds].revents = 0;
        nfds++;
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
}

/* ── Transport progress ─────────────────────────────────────────────── */

void chess_net_reset_transport_progress(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->network_session.connect_attempted = false;
    ctx->network_session.hello_sent = false;
    ctx->network_session.hello_received = false;
    ctx->network_session.hello_completed = false;
    ctx->network_session.challenge_done = false;
    ctx->network_session.start_sent = false;
    ctx->network_session.start_sent_at_ms = 0;
    ctx->network_session.resume_request_sent = false;
    ctx->network_session.pending_resume_state_sync = false;
}

/* ── Packet receive ─────────────────────────────────────────────────── */

static bool net_receive_next_packet(AppContext *ctx, ChessPacketHeader *header, uint8_t *payload, size_t payload_capacity)
{
    if (!ctx || !header || !payload) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(&ctx->connection, 50, header)) {
        SDL_Log("NET: failed to read packet header, closing connection");
        app_handle_peer_disconnect(ctx, "failed to read packet header");
        return false;
    }

    if (header->payload_size > payload_capacity) {
        SDL_Log("NET: received oversized payload (%u), closing connection", (unsigned)header->payload_size);
        app_handle_peer_disconnect(ctx, "received oversized payload");
        return false;
    }

    if (header->payload_size > 0u && !chess_tcp_recv_payload(&ctx->connection, 50, payload, header->payload_size)) {
        SDL_Log("NET: failed to read packet payload (type=%u size=%u), closing connection",
                (unsigned)header->message_type, (unsigned)header->payload_size);
        app_handle_peer_disconnect(ctx, "failed to read packet payload");
        return false;
    }

    return true;
}

/* ── Incoming packet handlers ───────────────────────────────────────── */

static void net_handle_hello_packet(AppContext *ctx, const ChessHelloPayload *hello)
{
    if (!ctx || !hello) {
        return;
    }

    ctx->network_session.hello_received = true;

    /* When the connection was accepted before mDNS discovery,
     * register minimal identity so that the later set_remote()
     * from mDNS sees same_remote == true and does not reset. */
    if (!ctx->network_session.peer_available && hello->profile_id[0] != '\0') {
        memset(&ctx->network_session.remote_peer, 0, sizeof(ctx->network_session.remote_peer));
        SDL_strlcpy(ctx->network_session.remote_peer.profile_id, hello->profile_id,
                     sizeof(ctx->network_session.remote_peer.profile_id));
        ctx->network_session.peer_available = true;
    }

    SDL_Log("NET: received HELLO from remote peer (%.8s...)", hello->profile_id);
}

static void net_handle_offer_packet(AppContext *ctx, const ChessOfferPayload *offer)
{
    int peer_idx = -1;
    int i;

    if (!ctx || !offer || ctx->network_session.challenge_done) {
        return;
    }

    SDL_Log("NET: received OFFER from remote peer (%.8s...)", offer->challenger_profile_id);

    for (i = 0; i < ctx->lobby.discovered_peer_count; ++i) {
        if (SDL_strncmp(ctx->lobby.discovered_peers[i].peer.profile_id, offer->challenger_profile_id, CHESS_PROFILE_ID_STRING_LEN) == 0) {
            peer_idx = i;
            break;
        }
    }

    if (peer_idx >= 0) {
        /* B2 fix: if we already sent an OFFER to this peer (cross-offer),
         * auto-accept instead of overwriting with INCOMING_PENDING.
         * Tiebreak: smaller profile_id => SERVER. */
        if (chess_lobby_get_challenge_state(&ctx->lobby, peer_idx) == CHESS_CHALLENGE_OUTGOING_PENDING) {
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_profile_id, ctx->network_session.local_peer.profile_id, sizeof(accept.acceptor_profile_id));
            if (ctx->connection.fd >= 0 && chess_tcp_send_accept(&ctx->connection, &accept)) {
                ctx->network_session.challenge_done = true;
                if (strncmp(ctx->network_session.local_peer.profile_id,
                            offer->challenger_profile_id,
                            CHESS_PROFILE_ID_STRING_LEN) < 0) {
                    ctx->network_session.role = CHESS_ROLE_SERVER;
                } else {
                    ctx->network_session.role = CHESS_ROLE_CLIENT;
                }
                chess_lobby_set_challenge_state(&ctx->lobby, peer_idx, CHESS_CHALLENGE_MATCHED);
                chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_GAME_STARTING);
                SDL_Log("NET: cross-offer detected, auto-accepted (%.8s...) role=%s",
                        offer->challenger_profile_id,
                        ctx->network_session.role == CHESS_ROLE_SERVER ? "SERVER" : "CLIENT");
            } else {
                chess_lobby_set_challenge_state(&ctx->lobby, peer_idx, CHESS_CHALLENGE_INCOMING_PENDING);
            }
        } else {
            chess_lobby_set_challenge_state(&ctx->lobby, peer_idx, CHESS_CHALLENGE_INCOMING_PENDING);
        }
        chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[peer_idx].peer);
    }
}

static void net_handle_accept_packet(AppContext *ctx, const ChessAcceptPayload *accept)
{
    int peer_idx = -1;
    int i;

    if (!ctx || !accept || ctx->network_session.challenge_done) {
        return;
    }

    /* B3 fix: lookup by acceptor profile_id first, fall back to selected_peer_idx. */
    for (i = 0; i < ctx->lobby.discovered_peer_count; ++i) {
        if (SDL_strncmp(
                ctx->lobby.discovered_peers[i].peer.profile_id,
                accept->acceptor_profile_id,
                CHESS_PROFILE_ID_STRING_LEN) == 0) {
            peer_idx = i;
            break;
        }
    }
    if (peer_idx < 0) {
        peer_idx = ctx->lobby.selected_peer_idx;
    }

    ctx->network_session.challenge_done = true;
    if (peer_idx >= 0) {
        chess_lobby_set_challenge_state(&ctx->lobby, peer_idx, CHESS_CHALLENGE_MATCHED);
    }
    chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_GAME_STARTING);

    SDL_Log("NET: received ACCEPT from remote peer (%.8s...)", accept->acceptor_profile_id);
    SDL_Log("NET: challenge exchange completed (remote accept), waiting START/ACK");
}

static void net_handle_start_packet(AppContext *ctx, const ChessStartPayload *start_payload)
{
    if (!ctx || !start_payload) {
        return;
    }

    if (ctx->network_session.role != CHESS_ROLE_CLIENT || ctx->network_session.start_completed) {
        return;
    }

    if (chess_tcp_send_ack(&ctx->connection, CHESS_MSG_START, 2u, 0u)) {
        chess_network_session_start_game(
            &ctx->network_session,
            start_payload->game_id,
            (ChessPlayerColor)start_payload->assigned_color
        );
        ctx->network_session.start_completed = true;
        ctx->pending_start_payload.game_id = start_payload->game_id;
        ctx->pending_start_payload.assigned_color = start_payload->assigned_color;
        ctx->pending_start_payload.initial_turn = start_payload->initial_turn;
        (void)snprintf(
            ctx->pending_start_payload.resume_token,
            sizeof(ctx->pending_start_payload.resume_token),
            "%s",
            start_payload->resume_token);

        chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_IN_GAME);

        /* Stop mDNS so this player is no longer listed in other lobbies
         * while in-game.  Restarted on return to lobby. */
        chess_discovery_stop(&ctx->discovery);

        if (!chess_persist_load_match_snapshot(ctx, start_payload->game_id, start_payload->resume_token)) {
            chess_game_state_init(&ctx->game_state);
            ctx->move_history_count = 0;
        } else {
            SDL_Log("GAME: restored snapshot for game_id=%u", start_payload->game_id);
        }

        (void)chess_persist_save_client_resume_state(ctx);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network_session.game_id,
            ctx->network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            start_payload->initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

static void net_handle_resume_request_packet(AppContext *ctx, const ChessResumeRequestPayload *request)
{
    ChessResumeResponsePayload response;
    char white_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char black_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char resume_token[CHESS_UUID_STRING_LEN];
    bool accepted = false;
    bool requester_is_white = false;

    if (!ctx || !request || ctx->network_session.role != CHESS_ROLE_SERVER) {
        return;
    }

    /* If the START/ACK exchange already completed, ignore late resume
     * requests — the game is in progress and resetting would break it. */
    if (ctx->network_session.start_completed) {
        SDL_Log("NET: ignoring late resume request (game already started)");
        return;
    }

    memset(&response, 0, sizeof(response));
    response.game_id = request->game_id;
    response.status = CHESS_RESUME_REJECTED;

    if (chess_persist_load_snapshot_metadata(
            request->game_id,
            white_profile_id,
            sizeof(white_profile_id),
            black_profile_id,
            sizeof(black_profile_id),
            resume_token,
            sizeof(resume_token))) {
        const bool token_matches = SDL_strncmp(resume_token, request->resume_token, CHESS_UUID_STRING_LEN) == 0;
        requester_is_white = SDL_strncmp(white_profile_id, request->profile_id, CHESS_PROFILE_ID_STRING_LEN) == 0;
        if (token_matches &&
            (requester_is_white ||
             SDL_strncmp(black_profile_id, request->profile_id, CHESS_PROFILE_ID_STRING_LEN) == 0)) {
            accepted = true;
        }
    }

    if (accepted) {
        response.status = CHESS_RESUME_ACCEPTED;
        memset(&ctx->pending_start_payload, 0, sizeof(ctx->pending_start_payload));
        ctx->pending_start_payload.game_id = request->game_id;
        ctx->pending_start_payload.initial_turn = CHESS_COLOR_WHITE;
        ctx->pending_start_payload.assigned_color = requester_is_white ? CHESS_COLOR_WHITE : CHESS_COLOR_BLACK;
        SDL_strlcpy(
            ctx->pending_start_payload.resume_token,
            request->resume_token,
            sizeof(ctx->pending_start_payload.resume_token));
        SDL_strlcpy(
            ctx->pending_start_payload.white_profile_id,
            requester_is_white ? ctx->network_session.remote_peer.profile_id : ctx->network_session.local_peer.profile_id,
            sizeof(ctx->pending_start_payload.white_profile_id));
        SDL_strlcpy(
            ctx->pending_start_payload.black_profile_id,
            requester_is_white ? ctx->network_session.local_peer.profile_id : ctx->network_session.remote_peer.profile_id,
            sizeof(ctx->pending_start_payload.black_profile_id));
        ctx->network_session.challenge_done = true;
        ctx->network_session.start_completed = false;
        ctx->network_session.start_sent = false;
        ctx->network_session.pending_resume_state_sync = true;
        chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_GAME_STARTING);
        SDL_Log("NET: resume request accepted for game %u", request->game_id);
    } else {
        ctx->network_session.pending_resume_state_sync = false;
        SDL_Log("NET: resume request rejected for game %u", request->game_id);
    }

    (void)chess_tcp_send_resume_response(&ctx->connection, &response);
}

static void net_handle_resume_response_packet(AppContext *ctx, const ChessResumeResponsePayload *response)
{
    if (!ctx || !response || ctx->network_session.role != CHESS_ROLE_CLIENT) {
        return;
    }

    if (!ctx->network_session.resume_request_sent) {
        return;
    }

    if (response->status == CHESS_RESUME_ACCEPTED) {
        app_set_status_message(ctx, "Reprise de partie acceptee, synchronisation...", 2500u);
        SDL_Log("NET: resume accepted for game %u", response->game_id);
    } else {
        chess_persist_clear_client_resume_state(ctx);
        app_set_status_message(ctx, "Reprise refusee, nouvelle partie requise.", 4000u);
        SDL_Log("NET: resume rejected for game %u", response->game_id);
    }
}

static void net_handle_state_snapshot_packet(AppContext *ctx, const ChessStateSnapshotPayload *snapshot)
{
    if (!ctx || !snapshot || ctx->network_session.role != CHESS_ROLE_CLIENT) {
        return;
    }

    if (!ctx->network_session.game_started ||
        ctx->network_session.game_id == 0u ||
        snapshot->game_id != ctx->network_session.game_id) {
        return;
    }

    if (!chess_persist_apply_state_snapshot_payload(ctx, snapshot, true)) {
        SDL_Log("NET: received invalid state snapshot for game %u", snapshot->game_id);
        return;
    }

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: applied synced snapshot for game_id=%u", snapshot->game_id);
    app_set_status_message(ctx, "Etat de partie resynchronise.", 2200u);
}

static void net_handle_ack_packet(AppContext *ctx, const ChessAckPayload *ack)
{
    if (!ctx || !ack || ctx->network_session.role != CHESS_ROLE_SERVER) {
        return;
    }

    if (ctx->network_session.start_sent &&
        !ctx->network_session.start_completed &&
        ack->acked_message_type == CHESS_MSG_START &&
        ack->acked_sequence == 2u &&
        ack->status_code == 0u) {
        SDL_Log("NET: START ACK received, switching to game view");
        chess_network_session_start_game(&ctx->network_session, ctx->pending_start_payload.game_id,
            opposite_color((ChessPlayerColor)ctx->pending_start_payload.assigned_color));
        ctx->network_session.start_completed = true;
        if (ctx->pending_start_payload.resume_token[0] == '\0') {
            (void)chess_generate_peer_uuid(
                ctx->pending_start_payload.resume_token,
                sizeof(ctx->pending_start_payload.resume_token));
        }

        chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_IN_GAME);

        /* Stop mDNS so this player is no longer listed in other lobbies
         * while in-game.  Restarted on return to lobby. */
        chess_discovery_stop(&ctx->discovery);

        if (!chess_persist_load_match_snapshot(
                ctx,
                ctx->pending_start_payload.game_id,
                ctx->pending_start_payload.resume_token)) {
            chess_game_state_init(&ctx->game_state);
            ctx->move_history_count = 0;
        } else {
            SDL_Log("GAME: restored snapshot for game_id=%u", ctx->pending_start_payload.game_id);
        }

        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network_session.game_id,
            ctx->network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            ctx->pending_start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

static void net_handle_move_packet(AppContext *ctx, const ChessMovePayload *move)
{
    ChessPiece moving_piece;
    ChessPlayerColor remote_color;
    char notation[24];
    bool notation_ready = false;

    if (!ctx || !move || !ctx->network_session.game_started) {
        return;
    }

    remote_color = opposite_color(ctx->network_session.local_color);
    if (remote_color == CHESS_COLOR_UNASSIGNED) {
        return;
    }

    moving_piece = chess_game_get_piece(&ctx->game_state, (int)move->from_file, (int)move->from_rank);

    if (chess_move_format_algebraic_notation(
            &ctx->game_state,
            (int)move->from_file,
            (int)move->from_rank,
            (int)move->to_file,
            (int)move->to_rank,
            move->promotion,
            notation,
            sizeof(notation))) {
        notation_ready = true;
    }

    if (chess_game_apply_remote_move(&ctx->game_state, remote_color, move)) {
        ChessPiece piece_to_animate = moving_piece;
        if (piece_to_animate == CHESS_PIECE_EMPTY) {
            piece_to_animate = chess_game_get_piece(&ctx->game_state, (int)move->to_file, (int)move->to_rank);
        }

        if (piece_to_animate != CHESS_PIECE_EMPTY) {
            ctx->remote_move_anim_active = true;
            ctx->remote_move_anim_piece = piece_to_animate;
            ctx->remote_move_from_file = (int)move->from_file;
            ctx->remote_move_from_rank = (int)move->from_rank;
            ctx->remote_move_to_file = (int)move->to_file;
            ctx->remote_move_to_rank = (int)move->to_rank;
            ctx->remote_move_anim_started_at_ms = SDL_GetTicks();
            if (ctx->remote_move_anim_duration_ms == 0u) {
                ctx->remote_move_anim_duration_ms = CHESS_REMOTE_MOVE_ANIM_DEFAULT_MS;
            }
        }

        if (notation_ready) {
            app_append_move_history(ctx, notation);
        }

        if (ctx->network_session.role == CHESS_ROLE_SERVER) {
            (void)chess_persist_save_match_snapshot(ctx);
        }

        SDL_Log(
            "GAME: applied remote move (%u,%u) -> (%u,%u)",
            (unsigned)move->from_file,
            (unsigned)move->from_rank,
            (unsigned)move->to_file,
            (unsigned)move->to_rank
        );
    } else {
        SDL_Log("GAME: ignoring invalid remote MOVE payload");
    }
}

/* ── Resign / Draw handlers ─────────────────────────────────────────── */

static void net_handle_resign_packet(AppContext *ctx)
{
    ChessPlayerColor remote_color;

    if (!ctx || !ctx->network_session.game_started ||
        ctx->game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    remote_color = opposite_color(ctx->network_session.local_color);
    ctx->game_state.outcome = (remote_color == CHESS_COLOR_WHITE)
        ? CHESS_OUTCOME_WHITE_RESIGNED
        : CHESS_OUTCOME_BLACK_RESIGNED;

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: opponent resigned");
    app_set_status_message(ctx, "Opponent resigned.", 5000u);
}

static void net_handle_draw_offer_packet(AppContext *ctx)
{
    if (!ctx || !ctx->network_session.game_started ||
        ctx->game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    ctx->network_session.draw_offer_received = true;
    SDL_Log("GAME: opponent offers a draw");
    app_set_status_message(ctx, "Draw offered — Accept or Decline.", 30000u);
}

static void net_handle_draw_accept_packet(AppContext *ctx)
{
    if (!ctx || !ctx->network_session.game_started ||
        ctx->game_state.outcome != CHESS_OUTCOME_NONE ||
        !ctx->network_session.draw_offer_pending) {
        return;
    }

    ctx->game_state.outcome = CHESS_OUTCOME_DRAW_AGREED;
    ctx->network_session.draw_offer_pending = false;

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: draw accepted");
    app_set_status_message(ctx, "Draw by agreement.", 5000u);
}

static void net_handle_draw_decline_packet(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->network_session.draw_offer_pending = false;
    SDL_Log("GAME: draw declined by opponent");
    app_set_status_message(ctx, "Draw declined.", 3000u);
}

/* ── Packet dispatch ────────────────────────────────────────────────── */

static void net_dispatch_incoming_packet(AppContext *ctx, const ChessPacketHeader *header, const uint8_t *payload)
{
    if (!ctx || !header || !payload) {
        return;
    }

    if (header->message_type == CHESS_MSG_HELLO && header->payload_size == sizeof(ChessHelloPayload)) {
        net_handle_hello_packet(ctx, (const ChessHelloPayload *)payload);
    } else if (header->message_type == CHESS_MSG_OFFER && header->payload_size == sizeof(ChessOfferPayload)) {
        net_handle_offer_packet(ctx, (const ChessOfferPayload *)payload);
    } else if (header->message_type == CHESS_MSG_ACCEPT && header->payload_size == sizeof(ChessAcceptPayload)) {
        net_handle_accept_packet(ctx, (const ChessAcceptPayload *)payload);
    } else if (header->message_type == CHESS_MSG_START && header->payload_size == sizeof(ChessStartPayload)) {
        net_handle_start_packet(ctx, (const ChessStartPayload *)payload);
    } else if (header->message_type == CHESS_MSG_ACK && header->payload_size == sizeof(ChessAckPayload)) {
        net_handle_ack_packet(ctx, (const ChessAckPayload *)payload);
    } else if (header->message_type == CHESS_MSG_MOVE && header->payload_size == sizeof(ChessMovePayload)) {
        net_handle_move_packet(ctx, (const ChessMovePayload *)payload);
    } else if (header->message_type == CHESS_MSG_RESUME_REQUEST &&
               header->payload_size == sizeof(ChessResumeRequestPayload)) {
        net_handle_resume_request_packet(ctx, (const ChessResumeRequestPayload *)payload);
    } else if (header->message_type == CHESS_MSG_RESUME_RESPONSE &&
               header->payload_size == sizeof(ChessResumeResponsePayload)) {
        net_handle_resume_response_packet(ctx, (const ChessResumeResponsePayload *)payload);
    } else if (header->message_type == CHESS_MSG_STATE_SNAPSHOT &&
               header->payload_size == sizeof(ChessStateSnapshotPayload)) {
        net_handle_state_snapshot_packet(ctx, (const ChessStateSnapshotPayload *)payload);
    } else if (header->message_type == CHESS_MSG_RESIGN && header->payload_size == 0u) {
        net_handle_resign_packet(ctx);
    } else if (header->message_type == CHESS_MSG_DRAW_OFFER && header->payload_size == 0u) {
        net_handle_draw_offer_packet(ctx);
    } else if (header->message_type == CHESS_MSG_DRAW_ACCEPT && header->payload_size == 0u) {
        net_handle_draw_accept_packet(ctx);
    } else if (header->message_type == CHESS_MSG_DRAW_DECLINE && header->payload_size == 0u) {
        net_handle_draw_decline_packet(ctx);
    }
}

static void net_drain_incoming_packets(AppContext *ctx)
{
    const int max_packets_per_frame = 8;
    int packet_idx;

    if (!ctx || ctx->connection.fd < 0) {
        return;
    }

    for (packet_idx = 0; packet_idx < max_packets_per_frame; ++packet_idx) {
        ChessSocketEvents drain_events;
        ChessPacketHeader header;
        uint8_t payload[sizeof(ChessStateSnapshotPayload)];

        poll_socket_events(&ctx->listener, &ctx->connection, &drain_events);
        if (!drain_events.connection_readable) {
            break;
        }

        if (!net_receive_next_packet(ctx, &header, payload, sizeof(payload))) {
            break;
        }

        net_dispatch_incoming_packet(ctx, &header, payload);
    }
}

/* ── Outgoing send helpers ──────────────────────────────────────────── */

static void net_send_state_snapshot_if_needed(AppContext *ctx)
{
    ChessStateSnapshotPayload snapshot;

    if (!ctx ||
        ctx->network_session.role != CHESS_ROLE_SERVER ||
        !ctx->network_session.pending_resume_state_sync ||
        !ctx->network_session.start_completed ||
        ctx->connection.fd < 0) {
        return;
    }

    if (!chess_persist_build_state_snapshot_payload(ctx, &snapshot)) {
        return;
    }

    if (chess_tcp_send_state_snapshot(&ctx->connection, &snapshot)) {
        ctx->network_session.pending_resume_state_sync = false;
        SDL_Log("NET: sent state snapshot for resumed game %u", snapshot.game_id);
    }
}

static void net_advance_transport_connection(AppContext *ctx, const ChessSocketEvents *socket_events)
{
    if (!ctx || !socket_events) {
        return;
    }

    if (ctx->network_session.hello_completed) {
        return;
    }

    /* Don't accept connections or initiate handshakes in post-game phases;
     * the user must click "Return to Lobby" first. */
    if (ctx->network_session.phase == CHESS_PHASE_DISCONNECTED ||
        ctx->network_session.phase == CHESS_PHASE_TERMINATED) {
        return;
    }

    /* Accept inbound TCP connections eagerly — even before mDNS
     * resolves the remote peer.  The HELLO handshake identifies
     * the peer.  This eliminates ~800 ms of Bonjour propagation
     * delay on reconnect. */
    if (ctx->connection.fd < 0 &&
        ctx->listener.fd >= 0 &&
        socket_events->listener_readable) {
        if (chess_tcp_accept_once(&ctx->listener, 0, &ctx->connection)) {
            SDL_Log("NET: accepted TCP client connection");
        }
    }

    /* Client outbound connect: requires role assignment (challenge accepted). */
    if (ctx->network_session.role != CHESS_ROLE_CLIENT &&
        ctx->network_session.role != CHESS_ROLE_SERVER) {
        return;
    }

    {
        const uint64_t now = SDL_GetTicks();
        bool should_attempt_client_connect = false;

        if (ctx->network_session.role == CHESS_ROLE_CLIENT) {
            if (!ctx->network_session.connect_attempted || now >= ctx->next_connect_attempt_at) {
                ctx->network_session.connect_attempted = true;
                ctx->next_connect_attempt_at = now + (uint64_t)ctx->connect_retry_ms;
                should_attempt_client_connect = true;
            }
        }

        if (ctx->network_session.role == CHESS_ROLE_CLIENT && should_attempt_client_connect) {
            uint32_t remote_ip = ctx->discovered_peer.tcp_ipv4;
            uint16_t remote_port = ctx->discovered_peer.tcp_port;
            if (ctx->lobby.selected_peer_idx >= 0 &&
                ctx->lobby.selected_peer_idx < ctx->lobby.discovered_peer_count) {
                remote_ip = ctx->lobby.discovered_peers[ctx->lobby.selected_peer_idx].tcp_ipv4;
                remote_port = ctx->lobby.discovered_peers[ctx->lobby.selected_peer_idx].tcp_port;
            }

            if (ctx->connection.fd < 0 &&
                chess_tcp_connect_once(
                    remote_ip,
                    remote_port,
                    200,
                    &ctx->connection
                )) {
                SDL_Log("NET: connected to remote TCP host");
            }
        }
    }
}

static void net_advance_hello_handshake(AppContext *ctx)
{
    ChessRole effective_role;

    if (!ctx || ctx->connection.fd < 0) {
        return;
    }

    if (ctx->network_session.hello_completed) {
        return;
    }

    /* Infer server role when we accepted an inbound connection
     * before the mDNS election completed (role still UNKNOWN). */
    effective_role = ctx->network_session.role;
    if (effective_role == CHESS_ROLE_UNKNOWN && !ctx->network_session.connect_attempted) {
        effective_role = CHESS_ROLE_SERVER;
    }

    if (effective_role == CHESS_ROLE_UNKNOWN) {
        return;
    }

    /* Commit inferred role immediately so that packet handlers
     * (e.g. net_handle_ack_packet) see the correct role when
     * processing packets later in the same tick. */
    if (ctx->network_session.role == CHESS_ROLE_UNKNOWN) {
        ctx->network_session.role = effective_role;
        SDL_Log("NET: inferred role %s from early accept",
                effective_role == CHESS_ROLE_SERVER ? "SERVER" : "CLIENT");
    }

    {
        ChessHelloPayload local_hello;
        memset(&local_hello, 0, sizeof(local_hello));
        SDL_strlcpy(local_hello.profile_id, ctx->network_session.local_peer.profile_id, sizeof(local_hello.profile_id));
        local_hello.role = (uint32_t)effective_role;

        /* CLIENT sends HELLO first; SERVER replies after receiving. */
        if (effective_role == CHESS_ROLE_CLIENT && !ctx->network_session.hello_sent) {
            if (chess_tcp_send_hello(&ctx->connection, &local_hello)) {
                ctx->network_session.hello_sent = true;
            }
        }

        if (effective_role == CHESS_ROLE_SERVER && ctx->network_session.hello_received && !ctx->network_session.hello_sent) {
            if (chess_tcp_send_hello(&ctx->connection, &local_hello)) {
                ctx->network_session.hello_sent = true;
            }
        }

        /* Both sides complete once they have sent AND received. */
        if (ctx->network_session.hello_sent && ctx->network_session.hello_received) {
            ctx->network_session.hello_completed = true;
            chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_AUTHENTICATED);
            SDL_Log("NET: HELLO handshake completed (%s)",
                    effective_role == CHESS_ROLE_SERVER ? "server" : "client");
        }
    }
}

static void net_send_pending_offer_if_needed(AppContext *ctx)
{
    if (!ctx || !ctx->network_session.hello_completed || ctx->network_session.challenge_done || ctx->connection.fd < 0) {
        return;
    }

    {
        const int peer_idx = ctx->lobby.selected_peer_idx;
        if (peer_idx >= 0 &&
            chess_lobby_get_challenge_state(&ctx->lobby, peer_idx) == CHESS_CHALLENGE_OUTGOING_PENDING &&
            !chess_lobby_has_offer_been_sent(&ctx->lobby, peer_idx)) {
            ChessOfferPayload offer;
            memset(&offer, 0, sizeof(offer));
            SDL_strlcpy(offer.challenger_profile_id, ctx->network_session.local_peer.profile_id, sizeof(offer.challenger_profile_id));
            if (chess_tcp_send_offer(&ctx->connection, &offer)) {
                chess_lobby_mark_offer_sent(&ctx->lobby, peer_idx);
                SDL_Log("NET: sent OFFER to selected peer");
            }
        }
    }
}

static void net_send_start_if_needed(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    if (!ctx->network_session.hello_completed ||
        !ctx->network_session.challenge_done ||
        ctx->network_session.start_completed ||
        ctx->network_session.role != CHESS_ROLE_SERVER ||
        ctx->connection.fd < 0 ||
        ctx->network_session.start_sent) {
        return;
    }

    if (ctx->pending_start_payload.game_id == 0u) {
        memset(&ctx->pending_start_payload, 0, sizeof(ctx->pending_start_payload));
        ctx->pending_start_payload.game_id = make_game_id(&ctx->network_session.local_peer, &ctx->network_session.remote_peer);
        ctx->pending_start_payload.initial_turn = CHESS_COLOR_WHITE;
        if (ctx->pending_start_payload.resume_token[0] == '\0') {
            (void)chess_generate_peer_uuid(
                ctx->pending_start_payload.resume_token,
                sizeof(ctx->pending_start_payload.resume_token));
        }

        {
            const bool server_is_white = (arc4random() % 2u) == 0u;
            if (server_is_white) {
                ctx->pending_start_payload.assigned_color = CHESS_COLOR_BLACK;
                SDL_strlcpy(ctx->pending_start_payload.white_profile_id, ctx->network_session.local_peer.profile_id, sizeof(ctx->pending_start_payload.white_profile_id));
                SDL_strlcpy(ctx->pending_start_payload.black_profile_id, ctx->network_session.remote_peer.profile_id, sizeof(ctx->pending_start_payload.black_profile_id));
            } else {
                ctx->pending_start_payload.assigned_color = CHESS_COLOR_WHITE;
                SDL_strlcpy(ctx->pending_start_payload.white_profile_id, ctx->network_session.remote_peer.profile_id, sizeof(ctx->pending_start_payload.white_profile_id));
                SDL_strlcpy(ctx->pending_start_payload.black_profile_id, ctx->network_session.local_peer.profile_id, sizeof(ctx->pending_start_payload.black_profile_id));
            }
            SDL_Log("NET: color assignment - server=%s, client=%s",
                server_is_white ? "WHITE" : "BLACK",
                server_is_white ? "BLACK" : "WHITE");
        }
    }

    if (chess_tcp_send_start(&ctx->connection, &ctx->pending_start_payload)) {
        ctx->network_session.start_sent = true;
        ctx->network_session.start_sent_at_ms = SDL_GetTicks();
    } else {
        ctx->network_session.start_failures += 1u;
        if (ctx->network_session.start_failures == 1u || (ctx->network_session.start_failures % 5u) == 0u) {
            SDL_Log("NET: START send failed (%u failures), will retry", ctx->network_session.start_failures);
        }
    }
}

static void net_send_resume_request_if_needed(AppContext *ctx)
{
    ChessResumeRequestPayload request;

    if (!ctx ||
        ctx->network_session.role != CHESS_ROLE_CLIENT ||
        !ctx->network_session.hello_completed ||
        ctx->connection.fd < 0 ||
        ctx->network_session.resume_request_sent ||
        ctx->network_session.start_completed ||
        ctx->pending_start_payload.game_id == 0u ||
        ctx->pending_start_payload.resume_token[0] == '\0') {
        return;
    }

    memset(&request, 0, sizeof(request));
    request.game_id = ctx->pending_start_payload.game_id;
    SDL_strlcpy(request.profile_id, ctx->local_peer.profile_id, sizeof(request.profile_id));
    SDL_strlcpy(request.resume_token, ctx->pending_start_payload.resume_token, sizeof(request.resume_token));

    if (chess_tcp_send_resume_request(&ctx->connection, &request)) {
        ctx->network_session.resume_request_sent = true;
        SDL_Log("NET: sent resume request for game %u", request.game_id);
    }
}

/* ── Public: network tick ───────────────────────────────────────────── */

void chess_net_tick(AppContext *ctx)
{
    ChessSocketEvents connection_phase_events;

    if (!ctx) {
        return;
    }

    /* B1 fix: retry START after 3 seconds if ACK never arrived. */
    if (ctx->network_session.start_sent && !ctx->network_session.start_completed &&
        (SDL_GetTicks() - ctx->network_session.start_sent_at_ms) > 3000u) {
        SDL_Log("NET: START ACK timeout, will retry");
        ctx->network_session.start_sent = false;
    }

    /* Timeout TCP_CONNECTING phase after 10 seconds. */
    if (ctx->network_session.phase == CHESS_PHASE_TCP_CONNECTING &&
        (SDL_GetTicks() - ctx->network_session.phase_entered_at_ms) > 10000u) {
        SDL_Log("NET: TCP_CONNECTING timeout, returning to IDLE");
        chess_tcp_connection_close(&ctx->connection);
        chess_net_reset_transport_progress(ctx);
        ctx->network_session.role = CHESS_ROLE_UNKNOWN;
        ctx->network_session.peer_available = false;
        memset(&ctx->network_session.remote_peer, 0, sizeof(ctx->network_session.remote_peer));
        chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_IDLE);
        if (ctx->lobby.selected_peer_idx >= 0) {
            chess_lobby_set_challenge_state(&ctx->lobby, ctx->lobby.selected_peer_idx, CHESS_CHALLENGE_NONE);
        }
        app_set_status_message(ctx, "Connection timeout. Try again.", 3000u);
    }

    poll_socket_events(&ctx->listener, &ctx->connection, &connection_phase_events);
    net_advance_transport_connection(ctx, &connection_phase_events);
    net_advance_hello_handshake(ctx);
    net_send_resume_request_if_needed(ctx);
    net_send_pending_offer_if_needed(ctx);

    net_drain_incoming_packets(ctx);
    net_send_start_if_needed(ctx);
    net_send_state_snapshot_if_needed(ctx);
}
