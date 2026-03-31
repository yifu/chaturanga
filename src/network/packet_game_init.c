/**
 * Game-initialization packet handlers: START, ACK, RESUME_REQUEST,
 * RESUME_RESPONSE, STATE_SNAPSHOT.
 *
 * Split from packet_handlers.c for focused module size.
 */
#include "packet_handlers_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/game_state.h"
#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/persistence.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* ── START ──────────────────────────────────────────────────────────── */

void chess_pkt_handle_start(AppContext *ctx, const ChessStartPayload *start_payload)
{
    if (!ctx || !start_payload) {
        return;
    }

    if (ctx->network.network_session.role != CHESS_ROLE_CLIENT || ctx->network.network_session.start_completed) {
        return;
    }

    if (transport_send_ack(&ctx->network.transport.base, CHESS_MSG_START, 2u, 0u)) {
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
            /* New game: initialize chess clocks from time control */
            ctx->game.time_control_ms = start_payload->time_control_ms;
            ctx->game.white_remaining_ms = start_payload->time_control_ms;
            ctx->game.black_remaining_ms = start_payload->time_control_ms;
        } else {
            /* Resumed: clocks restored by chess_persist_load_match_snapshot */
            SDL_Log("GAME: restored snapshot for game_id=%u", start_payload->game_id);
        }

        /* Initialize clock sync point */
        ctx->game.last_clock_sync_ticks = SDL_GetTicks();
        ctx->game.turn_started_at_ms = 0;

        (void)chess_persist_save_client_resume_state(ctx);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network.network_session.game_id,
            ctx->network.network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            start_payload->initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

/* ── ACK ────────────────────────────────────────────────────────────── */

void chess_pkt_handle_ack(AppContext *ctx, const ChessAckPayload *ack)
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
            chess_pkt_opposite_color((ChessPlayerColor)ctx->protocol.pending_start_payload.assigned_color));
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
            /* New game: initialize chess clocks from time control */
            ctx->game.time_control_ms = ctx->protocol.pending_start_payload.time_control_ms;
            ctx->game.white_remaining_ms = ctx->game.time_control_ms;
            ctx->game.black_remaining_ms = ctx->game.time_control_ms;
        } else {
            /* Resumed: clocks restored by chess_persist_load_match_snapshot */
            SDL_Log("GAME: restored snapshot for game_id=%u", ctx->protocol.pending_start_payload.game_id);
        }

        /* Initialize chess clocks */
        ctx->game.last_clock_sync_ticks = SDL_GetTicks();
        ctx->game.turn_started_at_ms = SDL_GetTicks();

        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log(
            "GAME: started (game_id=%u, local_color=%s, first_turn=%s)",
            ctx->network.network_session.game_id,
            ctx->network.network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
            ctx->protocol.pending_start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
        );
    }
}

/* ── RESUME_REQUEST ─────────────────────────────────────────────────── */

void chess_pkt_handle_resume_request(AppContext *ctx, const ChessResumeRequestPayload *request)
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

    if (!transport_send_resume_response(&ctx->network.transport.base, &response)) {
        SDL_Log("NET: failed to send resume response, disconnecting");
        app_handle_peer_disconnect(ctx, "failed to send RESUME_RESPONSE");
    }
}

/* ── RESUME_RESPONSE ────────────────────────────────────────────────── */

void chess_pkt_handle_resume_response(AppContext *ctx, const ChessResumeResponsePayload *response)
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

/* ── STATE_SNAPSHOT ──────────────────────────────────────────────────── */

void chess_pkt_handle_state_snapshot(AppContext *ctx, const ChessStateSnapshotPayload *snapshot)
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
