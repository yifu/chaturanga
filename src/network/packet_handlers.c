#include "chess_app/net_handler.h"

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

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

/* ── Packet receive ─────────────────────────────────────────────────── */

static ChessRecvResult net_receive_next_packet(AppContext *ctx, ChessPacketHeader *header, uint8_t *payload, size_t payload_capacity)
{
    ChessRecvResult result;

    if (!ctx || !header || !payload) {
        return CHESS_RECV_ERROR;
    }

    result = chess_tcp_recv_nonblocking(&ctx->network.connection, &ctx->network.recv_buffer, header, payload, payload_capacity);

    if (result == CHESS_RECV_ERROR) {
        SDL_Log("NET: recv error on game connection, closing");
        app_handle_peer_disconnect(ctx, "recv error on game connection");
    }

    return result;
}

/* ── Incoming packet handlers ───────────────────────────────────────── */

static void net_handle_hello_packet(AppContext *ctx, const ChessHelloPayload *hello)
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

static void net_handle_offer_packet(AppContext *ctx, const ChessOfferPayload *offer)
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
         * Use ctx->network.connection (the server-side connection) for the game. */
        if (chess_lobby_get_challenge_state(&ctx->game.lobby, peer_idx) == CHESS_CHALLENGE_OUTGOING_PENDING) {
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(accept.acceptor_profile_id));
            if (ctx->network.connection.fd >= 0 && chess_tcp_send_accept(&ctx->network.connection, &accept)) {
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

static void net_handle_accept_packet(AppContext *ctx, const ChessAcceptPayload *accept)
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

static void net_handle_start_packet(AppContext *ctx, const ChessStartPayload *start_payload)
{
    if (!ctx || !start_payload) {
        return;
    }

    if (ctx->network.network_session.role != CHESS_ROLE_CLIENT || ctx->network.network_session.start_completed) {
        return;
    }

    if (chess_tcp_send_ack(&ctx->network.connection, CHESS_MSG_START, 2u, 0u)) {
        chess_network_session_start_game(
            &ctx->network.network_session,
            start_payload->game_id,
            (ChessPlayerColor)start_payload->assigned_color
        );
        ctx->network.network_session.start_completed = true;
        ctx->protocol.pending_start_payload.game_id = start_payload->game_id;
        ctx->protocol.pending_start_payload.assigned_color = start_payload->assigned_color;
        ctx->protocol.pending_start_payload.initial_turn = start_payload->initial_turn;
        (void)snprintf(
            ctx->protocol.pending_start_payload.resume_token,
            sizeof(ctx->protocol.pending_start_payload.resume_token),
            "%s",
            start_payload->resume_token);

        chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_IN_GAME);

        /* Stop mDNS so this player is no longer listed in other lobbies
         * while in-game.  Restarted on return to lobby. */
        chess_discovery_stop(&ctx->network.discovery);

        if (!chess_persist_load_match_snapshot(ctx, start_payload->game_id, start_payload->resume_token)) {
            chess_game_state_init(&ctx->game.game_state);
            ctx->game.move_history_count = 0;
        } else {
            SDL_Log("GAME: restored snapshot for game_id=%u", start_payload->game_id);
        }

        (void)chess_persist_save_client_resume_state(ctx);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network.network_session.game_id,
            ctx->network.network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
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

    if (!ctx || !request || ctx->network.network_session.role != CHESS_ROLE_SERVER) {
        return;
    }

    /* If the START/ACK exchange already completed, ignore late resume
     * requests — the game is in progress and resetting would break it. */
    if (ctx->network.network_session.start_completed) {
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
        memset(&ctx->protocol.pending_start_payload, 0, sizeof(ctx->protocol.pending_start_payload));
        ctx->protocol.pending_start_payload.game_id = request->game_id;
        ctx->protocol.pending_start_payload.initial_turn = CHESS_COLOR_WHITE;
        ctx->protocol.pending_start_payload.assigned_color = requester_is_white ? CHESS_COLOR_WHITE : CHESS_COLOR_BLACK;
        SDL_strlcpy(
            ctx->protocol.pending_start_payload.resume_token,
            request->resume_token,
            sizeof(ctx->protocol.pending_start_payload.resume_token));
        SDL_strlcpy(
            ctx->protocol.pending_start_payload.white_profile_id,
            requester_is_white ? ctx->network.network_session.remote_peer.profile_id : ctx->network.network_session.local_peer.profile_id,
            sizeof(ctx->protocol.pending_start_payload.white_profile_id));
        SDL_strlcpy(
            ctx->protocol.pending_start_payload.black_profile_id,
            requester_is_white ? ctx->network.network_session.local_peer.profile_id : ctx->network.network_session.remote_peer.profile_id,
            sizeof(ctx->protocol.pending_start_payload.black_profile_id));
        ctx->network.network_session.challenge_done = true;
        ctx->network.network_session.start_completed = false;
        ctx->network.network_session.start_sent = false;
        ctx->network.network_session.pending_resume_state_sync = true;
        chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_GAME_STARTING);
        SDL_Log("NET: resume request accepted for game %u", request->game_id);
    } else {
        ctx->network.network_session.pending_resume_state_sync = false;
        SDL_Log("NET: resume request rejected for game %u", request->game_id);
    }

    if (!chess_tcp_send_resume_response(&ctx->network.connection, &response)) {
        SDL_Log("NET: failed to send resume response, disconnecting");
        app_handle_peer_disconnect(ctx, "failed to send RESUME_RESPONSE");
    }
}

static void net_handle_resume_response_packet(AppContext *ctx, const ChessResumeResponsePayload *response)
{
    if (!ctx || !response || ctx->network.network_session.role != CHESS_ROLE_CLIENT) {
        return;
    }

    if (!ctx->network.network_session.resume_request_sent) {
        return;
    }

    if (response->status == CHESS_RESUME_ACCEPTED) {
        app_set_status_message(ctx, "Resume accepted, synchronizing game state...", 2500u);
        SDL_Log("NET: resume accepted for game %u", response->game_id);
    } else {
        chess_persist_clear_client_resume_state(ctx);
        app_set_status_message(ctx, "Resume rejected, a new game is required.", 4000u);
        SDL_Log("NET: resume rejected for game %u", response->game_id);
    }
}

static void net_handle_state_snapshot_packet(AppContext *ctx, const ChessStateSnapshotPayload *snapshot)
{
    if (!ctx || !snapshot || ctx->network.network_session.role != CHESS_ROLE_CLIENT) {
        return;
    }

    if (!ctx->network.network_session.game_started ||
        ctx->network.network_session.game_id == 0u ||
        snapshot->game_id != ctx->network.network_session.game_id) {
        return;
    }

    if (!chess_persist_apply_state_snapshot_payload(ctx, snapshot, true)) {
        SDL_Log("NET: received invalid state snapshot for game %u", snapshot->game_id);
        return;
    }

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: applied synced snapshot for game_id=%u", snapshot->game_id);
    app_set_status_message(ctx, "Game state re-synchronized.", 2200u);
}

static void net_handle_ack_packet(AppContext *ctx, const ChessAckPayload *ack)
{
    if (!ctx || !ack || ctx->network.network_session.role != CHESS_ROLE_SERVER) {
        return;
    }

    if (ctx->network.network_session.start_sent &&
        !ctx->network.network_session.start_completed &&
        ack->acked_message_type == CHESS_MSG_START &&
        ack->acked_sequence == 2u &&
        ack->status_code == 0u) {
        SDL_Log("NET: START ACK received, switching to game view");
        chess_network_session_start_game(&ctx->network.network_session, ctx->protocol.pending_start_payload.game_id,
            opposite_color((ChessPlayerColor)ctx->protocol.pending_start_payload.assigned_color));
        ctx->network.network_session.start_completed = true;
        if (ctx->protocol.pending_start_payload.resume_token[0] == '\0') {
            (void)chess_generate_peer_uuid(
                ctx->protocol.pending_start_payload.resume_token,
                sizeof(ctx->protocol.pending_start_payload.resume_token));
        }

        chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_IN_GAME);

        /* Stop mDNS so this player is no longer listed in other lobbies
         * while in-game.  Restarted on return to lobby. */
        chess_discovery_stop(&ctx->network.discovery);

        if (!chess_persist_load_match_snapshot(
                ctx,
                ctx->protocol.pending_start_payload.game_id,
                ctx->protocol.pending_start_payload.resume_token)) {
            chess_game_state_init(&ctx->game.game_state);
            ctx->game.move_history_count = 0;
        } else {
            SDL_Log("GAME: restored snapshot for game_id=%u", ctx->protocol.pending_start_payload.game_id);
        }

        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network.network_session.game_id,
            ctx->network.network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            ctx->protocol.pending_start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

static void net_handle_move_packet(AppContext *ctx, const ChessMovePayload *move)
{
    ChessPiece moving_piece;
    ChessPlayerColor remote_color;
    char notation[24];
    bool notation_ready = false;

    if (!ctx || !move || !ctx->network.network_session.game_started) {
        return;
    }

    remote_color = opposite_color(ctx->network.network_session.local_color);
    if (remote_color == CHESS_COLOR_UNASSIGNED) {
        return;
    }

    moving_piece = chess_game_get_piece(&ctx->game.game_state, (int)move->from_file, (int)move->from_rank);

    if (chess_move_format_algebraic_notation(
            &ctx->game.game_state,
            (int)move->from_file,
            (int)move->from_rank,
            (int)move->to_file,
            (int)move->to_rank,
            move->promotion,
            notation,
            sizeof(notation))) {
        notation_ready = true;
    }

    /* Detect capture before the move modifies the board */
    {
        ChessPiece victim = chess_game_get_piece(
            &ctx->game.game_state, (int)move->to_file, (int)move->to_rank);
        int victim_file = (int)move->to_file;
        int victim_rank = (int)move->to_rank;

        if (victim == CHESS_PIECE_EMPTY) {
            /* En passant: pawn moving diagonally to empty square */
            ChessPiece mover = chess_game_get_piece(
                &ctx->game.game_state, (int)move->from_file, (int)move->from_rank);
            if ((mover == CHESS_PIECE_WHITE_PAWN || mover == CHESS_PIECE_BLACK_PAWN) &&
                move->to_file != move->from_file) {
                victim_rank = (int)move->from_rank;
                victim = chess_game_get_piece(
                    &ctx->game.game_state, (int)move->to_file, victim_rank);
            }
        }

        if (chess_game_apply_remote_move(&ctx->game.game_state, remote_color, move)) {
            if (victim != CHESS_PIECE_EMPTY) {
                chess_ui_start_capture_animation(
                    ctx, victim, victim_file, victim_rank);
            }

            ChessPiece piece_to_animate = moving_piece;
            if (piece_to_animate == CHESS_PIECE_EMPTY) {
                piece_to_animate = chess_game_get_piece(&ctx->game.game_state, (int)move->to_file, (int)move->to_rank);
            }

            if (piece_to_animate != CHESS_PIECE_EMPTY) {
                ctx->ui.remote_move_anim.active = true;
                ctx->ui.remote_move_anim.piece = piece_to_animate;
                ctx->ui.remote_move_anim.from_file = (int)move->from_file;
                ctx->ui.remote_move_anim.from_rank = (int)move->from_rank;
                ctx->ui.remote_move_anim.to_file = (int)move->to_file;
                ctx->ui.remote_move_anim.to_rank = (int)move->to_rank;
                ctx->ui.remote_move_anim.started_at_ms = SDL_GetTicks();
                if (ctx->ui.remote_move_anim.duration_ms == 0u) {
                    ctx->ui.remote_move_anim.duration_ms = CHESS_REMOTE_MOVE_ANIM_DEFAULT_MS;
                }
            }

            if (notation_ready) {
                app_append_move_history(ctx, notation);
            }

            if (ctx->network.network_session.role == CHESS_ROLE_SERVER) {
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
}

/* ── Resign / Draw handlers ─────────────────────────────────────────── */

static void net_handle_resign_packet(AppContext *ctx)
{
    ChessPlayerColor remote_color;

    if (!ctx || !ctx->network.network_session.game_started ||
        ctx->game.game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    remote_color = opposite_color(ctx->network.network_session.local_color);
    ctx->game.game_state.outcome = (remote_color == CHESS_COLOR_WHITE)
        ? CHESS_OUTCOME_WHITE_RESIGNED
        : CHESS_OUTCOME_BLACK_RESIGNED;

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: opponent resigned");
    app_set_status_message(ctx, "Opponent resigned.", 5000u);
}

static void net_handle_draw_offer_packet(AppContext *ctx)
{
    if (!ctx || !ctx->network.network_session.game_started ||
        ctx->game.game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    ctx->network.network_session.draw_offer_received = true;
    SDL_Log("GAME: opponent offers a draw");
    app_set_status_message(ctx, "Draw offered — Accept or Decline.", 30000u);
}

static void net_handle_draw_accept_packet(AppContext *ctx)
{
    if (!ctx || !ctx->network.network_session.game_started ||
        ctx->game.game_state.outcome != CHESS_OUTCOME_NONE ||
        !ctx->network.network_session.draw_offer_pending) {
        return;
    }

    ctx->game.game_state.outcome = CHESS_OUTCOME_DRAW_AGREED;
    ctx->network.network_session.draw_offer_pending = false;

    (void)chess_persist_save_match_snapshot(ctx);
    SDL_Log("GAME: draw accepted");
    app_set_status_message(ctx, "Draw by agreement.", 5000u);
}

static void net_handle_draw_decline_packet(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->network.network_session.draw_offer_pending = false;
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


void chess_net_drain_incoming_packets(AppContext *ctx, bool initially_readable)
{
    const int max_packets_per_frame = 8;
    int packet_idx;

    if (!ctx || ctx->network.connection.fd < 0 || !initially_readable) {
        return;
    }

    for (packet_idx = 0; packet_idx < max_packets_per_frame; ++packet_idx) {
        ChessPacketHeader header;
        uint8_t payload[sizeof(ChessStateSnapshotPayload)];
        ChessRecvResult result = net_receive_next_packet(ctx, &header, payload, sizeof(payload));

        if (result != CHESS_RECV_OK) {
            break;
        }

        net_dispatch_incoming_packet(ctx, &header, payload);
    }
}
