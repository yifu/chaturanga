#include "chess_app/app_context.h"
#include "chess_app/net_handler.h"
#include "chess_app/persistence.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_discovery.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/transport.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---------- static constants ---------- */

static const int s_history_max_entries = (int)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES;
static const int s_history_entry_len  = (int)CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN;

/* ── Status message ──────────────────────────────────────────────────── */

void app_set_status_message(AppContext *ctx, const char *message, uint32_t duration_ms)
{
    if (!ctx || !message) {
        return;
    }

    SDL_strlcpy(ctx->ui.status_message, message, sizeof(ctx->ui.status_message));
    ctx->ui.status_message_until_ms = SDL_GetTicks() + (uint64_t)duration_ms;
}

/* ── Move history ────────────────────────────────────────────────────── */

void app_append_move_history(AppContext *ctx, const char *notation)
{
    int idx;

    if (!ctx || !notation || notation[0] == '\0') {
        return;
    }

    if (ctx->game.move_history_count < (uint16_t)s_history_max_entries) {
        SDL_strlcpy(
            ctx->game.move_history[ctx->game.move_history_count],
            notation,
            (size_t)s_history_entry_len);
        ctx->game.move_history_count += 1u;
        return;
    }

    for (idx = 1; idx < s_history_max_entries; ++idx) {
        SDL_memcpy(
            ctx->game.move_history[idx - 1],
            ctx->game.move_history[idx],
            (size_t)s_history_entry_len);
    }
    SDL_strlcpy(ctx->game.move_history[s_history_max_entries - 1], notation, (size_t)s_history_entry_len);
}

/* ── Challenge cleanup ───────────────────────────────────────────────── */

static void app_clear_challenges(AppContext *ctx)
{
    int i;

    if (!ctx) {
        return;
    }

    chess_lobby_close_all_challenge_connections(&ctx->game.lobby);
    for (i = 0; i < ctx->game.lobby.discovered_peer_count; ++i) {
        chess_lobby_set_challenge_state(&ctx->game.lobby, i, CHESS_CHALLENGE_NONE);
    }
}

/* ── Peer disconnect ─────────────────────────────────────────────────── */

void app_handle_peer_disconnect(AppContext *ctx, const char *reason)
{
    char remote_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    bool was_in_game;
    bool game_over;
    uint32_t last_game_id = 0u;
    char last_resume_token[CHESS_UUID_STRING_LEN] = {0};

    if (!ctx) {
        return;
    }

    was_in_game = ctx->network.network_session.game_started;
    game_over   = (ctx->game.game_state.outcome != CHESS_OUTCOME_NONE);
    if (was_in_game) {
        last_game_id = ctx->network.network_session.game_id;
        SDL_strlcpy(last_resume_token, ctx->protocol.pending_start_payload.resume_token, sizeof(last_resume_token));
    }
    SDL_strlcpy(remote_profile_id, ctx->network.network_session.remote_peer.profile_id, sizeof(remote_profile_id));

    if (reason && reason[0] != '\0') {
        SDL_Log("NET: peer disconnected (%s)", reason);
    } else {
        SDL_Log("NET: peer disconnected");
    }

    transport_close(&ctx->network.transport.base);
    transport_recv_reset(&ctx->network.transport.base);
    chess_net_reset_transport_progress(ctx);

    /*
     * If the game is over, keep the game state + overlay visible so the
     * player can review the result and click "Return to Lobby" themselves.
     * Only clean up networking state.
     */
    if (was_in_game && game_over) {
        ctx->network.network_session.transport_connected = false;
        ctx->network.network_session.peer_available = false;
        chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_DISCONNECTED);
        return;
    }

    ctx->network.network_session.start_completed = false;
    ctx->network.network_session.start_failures = 0u;
    memset(&ctx->protocol.pending_start_payload, 0, sizeof(ctx->protocol.pending_start_payload));
    if (was_in_game && last_game_id != 0u && last_resume_token[0] != '\0') {
        ctx->protocol.pending_start_payload.game_id = last_game_id;
        SDL_strlcpy(
            ctx->protocol.pending_start_payload.resume_token,
            last_resume_token,
            sizeof(ctx->protocol.pending_start_payload.resume_token));
    }
    ctx->network.network_session.peer_available = false;
    ctx->network.network_session.game_started = false;
    ctx->network.network_session.role = CHESS_ROLE_UNKNOWN;
    memset(&ctx->network.network_session.remote_peer, 0, sizeof(ctx->network.network_session.remote_peer));
    chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_IDLE);
    memset(&ctx->network.discovered_peer, 0, sizeof(ctx->network.discovered_peer));
    ctx->network.discovery.remote_emitted = false;

    /* Restart mDNS so this player is visible in the lobby again and can
     * rediscover the opponent for a potential resume. */
    chess_discovery_stop(&ctx->network.discovery);
    chess_discovery_start(&ctx->network.discovery, &ctx->network.local_peer, ctx->network.listener.port);
    ctx->ui.drag.drag_active = false;
    ctx->ui.drag.drag_piece = CHESS_PIECE_EMPTY;
    ctx->ui.drag.promotion_pending = false;
    ctx->ui.drag.promotion_to_file = -1;
    ctx->ui.drag.promotion_to_rank = -1;
    ctx->ui.remote_move_anim.active = false;
    ctx->ui.remote_move_anim.piece = CHESS_PIECE_EMPTY;
    chess_game_clear_selection(&ctx->game.game_state);
    ctx->game.move_history_count = 0;
    if (was_in_game) {
        (void)chess_lobby_remove_peer_by_profile_id(&ctx->game.lobby, remote_profile_id);
    }
    app_clear_challenges(ctx);

    if (was_in_game) {
        app_set_status_message(
            ctx,
            "Opponent disconnected. Attempting resumable reconnect.",
            5000u
        );
    } else {
        app_set_status_message(
            ctx,
            "Peer disconnected. Waiting for discovery updates.",
            3000u
        );
    }
}

/* ── Return to lobby ─────────────────────────────────────────────────── */

void app_return_to_lobby(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    SDL_Log("GAME: returning to lobby");

    transport_close(&ctx->network.transport.base);
    transport_recv_reset(&ctx->network.transport.base);
    chess_lobby_close_all_challenge_connections(&ctx->game.lobby);
    chess_net_reset_transport_progress(ctx);
    chess_persist_clear_client_resume_state(ctx);

    memset(&ctx->protocol.pending_start_payload, 0, sizeof(ctx->protocol.pending_start_payload));
    memset(&ctx->network.discovered_peer, 0, sizeof(ctx->network.discovered_peer));

    /* Restart mDNS: de-register stale service + re-advertise on same port */
    chess_discovery_stop(&ctx->network.discovery);
    chess_discovery_start(&ctx->network.discovery, &ctx->network.local_peer, ctx->network.listener.port);

    /* Full session reset (phase, flags, role, draw state, etc.) */
    chess_network_session_init(&ctx->network.network_session, &ctx->network.local_peer);

    ctx->ui.drag.drag_active = false;
    ctx->ui.drag.drag_piece = CHESS_PIECE_EMPTY;
    ctx->ui.drag.promotion_pending = false;
    ctx->ui.drag.promotion_to_file = -1;
    ctx->ui.drag.promotion_to_rank = -1;
    ctx->ui.remote_move_anim.active = false;
    ctx->ui.remote_move_anim.piece = CHESS_PIECE_EMPTY;

    chess_game_state_init(&ctx->game.game_state);
    ctx->game.move_history_count = 0;
    chess_lobby_init(&ctx->game.lobby);

    app_set_status_message(ctx, "Returned to lobby.", 2000u);
}
