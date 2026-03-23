#include "chess_app/app.h"
#include "chess_app/app_context.h"
#include "chess_app/input_handler.h"
#include "chess_app/net_handler.h"
#include "chess_app/persistence.h"
#include "chess_app/ui_fonts.h"
#include "chess_app/ui_game.h"
#include "chess_app/ui_lobby.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---------- static constants ---------- */

static const int s_history_max_entries = (int)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES;
static const int s_history_entry_len  = (int)CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN;

static bool init_local_peer(ChessPeerInfo *local_peer)
{
    if (!local_peer) {
        return false;
    }

    if (!chess_peer_init_local_identity(local_peer)) {
        SDL_Log("NET: could not initialize local peer identity");
        return false;
    }

        SDL_Log("NET: local peer initialized (uuid=%s display=%s@%s)",
            local_peer->uuid,
            local_peer->username,
            local_peer->hostname);
    return true;
}

/* AppLoopContext is now defined in chess_app/app_context.h */
/* Persistence functions moved to persistence.c */
/* Lobby rendering moved to ui/lobby.c */
/* Game UI rendering moved to ui/game.c */

/* App context and startup helpers */
static void app_loop_context_init_defaults(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->window_size = 640;
    ctx->connect_retry_ms = 1000;
    ctx->connection.fd = -1;
    ctx->running = true;
}

static void app_loop_context_shutdown(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    chess_discovery_stop(&ctx->discovery);
    chess_tcp_connection_close(&ctx->connection);
    chess_tcp_listener_close(&ctx->listener);
    destroy_piece_textures();
    if (ctx->renderer) {
        SDL_DestroyRenderer(ctx->renderer);
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
    }
    SDL_Quit();
}

static bool app_init_window_and_renderer(AppLoopContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("APP: SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    ctx->window = SDL_CreateWindow(
        "SDL3 Chess Board",
        ctx->window_size + CHESS_UI_HISTORY_PANEL_WIDTH,
        ctx->window_size,
        0);
    if (!ctx->window) {
        SDL_Log("APP: SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        SDL_Log("APP: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
        SDL_Quit();
        return false;
    }

    init_piece_textures(ctx->renderer);
    return true;
}

static void app_init_runtime_state(AppLoopContext *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->connection.fd = -1;
    memset(&ctx->discovered_peer, 0, sizeof(ctx->discovered_peer));
    ctx->start_completed = false;
    memset(&ctx->pending_start_payload, 0, sizeof(ctx->pending_start_payload));
    ctx->start_failures = 0u;
    ctx->move_sequence = 3u;
    ctx->next_connect_attempt_at = 0;
    ctx->resume_state_loaded = false;
    ctx->pending_resume_state_sync = false;
    ctx->resume_remote_profile_id[0] = '\0';
    ctx->drag_active = false;
    ctx->drag_piece = CHESS_PIECE_EMPTY;
    ctx->drag_from_file = -1;
    ctx->drag_from_rank = -1;
    ctx->drag_mouse_x = 0;
    ctx->drag_mouse_y = 0;
    ctx->promotion_pending = false;
    ctx->promotion_to_file = -1;
    ctx->promotion_to_rank = -1;
    ctx->remote_move_anim_active = false;
    ctx->remote_move_anim_piece = CHESS_PIECE_EMPTY;
    ctx->remote_move_from_file = -1;
    ctx->remote_move_from_rank = -1;
    ctx->remote_move_to_file = -1;
    ctx->remote_move_to_rank = -1;
    ctx->remote_move_anim_started_at_ms = 0;
    ctx->remote_move_anim_duration_ms = CHESS_REMOTE_MOVE_ANIM_DEFAULT_MS;
    ctx->status_message[0] = '\0';
    ctx->status_message_until_ms = 0;
    ctx->move_history_count = 0;

    chess_net_reset_transport_progress(ctx);
    chess_game_state_init(&ctx->game_state);
    chess_lobby_init(&ctx->lobby);
}

void app_set_status_message(AppLoopContext *ctx, const char *message, uint32_t duration_ms)
{
    if (!ctx || !message) {
        return;
    }

    SDL_strlcpy(ctx->status_message, message, sizeof(ctx->status_message));
    ctx->status_message_until_ms = SDL_GetTicks() + (uint64_t)duration_ms;
}

void app_append_move_history(AppLoopContext *ctx, const char *notation)
{
    int idx;

    if (!ctx || !notation || notation[0] == '\0') {
        return;
    }

    if (ctx->move_history_count < (uint16_t)s_history_max_entries) {
        SDL_strlcpy(
            ctx->move_history[ctx->move_history_count],
            notation,
            (size_t)s_history_entry_len);
        ctx->move_history_count += 1u;
        return;
    }

    for (idx = 1; idx < s_history_max_entries; ++idx) {
        SDL_memcpy(
            ctx->move_history[idx - 1],
            ctx->move_history[idx],
            (size_t)s_history_entry_len);
    }
    SDL_strlcpy(ctx->move_history[s_history_max_entries - 1], notation, (size_t)s_history_entry_len);
}

static void app_clear_challenges(AppLoopContext *ctx)
{
    int i;

    if (!ctx) {
        return;
    }

    for (i = 0; i < ctx->lobby.discovered_peer_count; ++i) {
        chess_lobby_set_challenge_state(&ctx->lobby, i, CHESS_CHALLENGE_NONE);
    }
}

void app_handle_peer_disconnect(AppLoopContext *ctx, const char *reason)
{
    char remote_uuid[CHESS_UUID_STRING_LEN];
    bool was_in_game;
    uint32_t last_game_id = 0u;
    char last_resume_token[CHESS_UUID_STRING_LEN] = {0};

    if (!ctx) {
        return;
    }

    was_in_game = ctx->network_session.game_started;
    if (was_in_game) {
        last_game_id = ctx->network_session.game_id;
        SDL_strlcpy(last_resume_token, ctx->pending_start_payload.resume_token, sizeof(last_resume_token));
    }
    SDL_strlcpy(remote_uuid, ctx->network_session.remote_peer.uuid, sizeof(remote_uuid));

    if (reason && reason[0] != '\0') {
        SDL_Log("NET: peer disconnected (%s)", reason);
    } else {
        SDL_Log("NET: peer disconnected");
    }

    chess_tcp_connection_close(&ctx->connection);
    chess_net_reset_transport_progress(ctx);

    ctx->start_completed = false;
    ctx->start_failures = 0u;
    memset(&ctx->pending_start_payload, 0, sizeof(ctx->pending_start_payload));
    if (was_in_game && last_game_id != 0u && last_resume_token[0] != '\0') {
        ctx->pending_start_payload.game_id = last_game_id;
        SDL_strlcpy(
            ctx->pending_start_payload.resume_token,
            last_resume_token,
            sizeof(ctx->pending_start_payload.resume_token));
    }
    ctx->network_session.peer_available = false;
    ctx->network_session.transport_ready = false;
    ctx->network_session.game_started = false;
    ctx->network_session.role = CHESS_ROLE_UNKNOWN;
    memset(&ctx->network_session.remote_peer, 0, sizeof(ctx->network_session.remote_peer));
    ctx->network_session.state = CHESS_NET_IDLE_DISCOVERY;
    memset(&ctx->discovered_peer, 0, sizeof(ctx->discovered_peer));
    ctx->discovery.remote_emitted = false;
    ctx->drag_active = false;
    ctx->drag_piece = CHESS_PIECE_EMPTY;
    ctx->promotion_pending = false;
    ctx->promotion_to_file = -1;
    ctx->promotion_to_rank = -1;
    ctx->remote_move_anim_active = false;
    ctx->remote_move_anim_piece = CHESS_PIECE_EMPTY;
    chess_game_clear_selection(&ctx->game_state);
    ctx->move_history_count = 0;
    (void)chess_lobby_remove_peer_by_uuid(&ctx->lobby, remote_uuid);
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

static bool app_init_networking(AppLoopContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!init_local_peer(&ctx->local_peer)) {
        return false;
    }

    if (!chess_tcp_listener_open(&ctx->listener, 0)) {
        SDL_Log("NET: could not create TCP listener on ephemeral port");
        return false;
    }

    SDL_Log("NET: listener ready on port %u", (unsigned int)ctx->listener.port);

    app_init_runtime_state(ctx);

    if (chess_persist_load_client_resume_state(ctx)) {
        SDL_Log(
            "NET: loaded persisted resume context (game_id=%u, remote_profile=%.8s...)",
            ctx->pending_start_payload.game_id,
            ctx->resume_remote_profile_id);
        app_set_status_message(ctx, "Reprise detectee: recherche de l'adversaire...", 3000u);
    }

    if (!chess_discovery_start(&ctx->discovery, &ctx->local_peer, ctx->listener.port)) {
        SDL_Log("NET: discovery start failed");
        return false;
    }

    chess_network_session_init(&ctx->network_session, &ctx->local_peer);
    return true;
}

/* App frame helpers */
static void app_poll_discovery_and_update_lobby(AppLoopContext *ctx)
{
    bool has_resume_target;

    if (!ctx) {
        return;
    }

    has_resume_target =
        ctx->resume_state_loaded &&
        ctx->pending_start_payload.game_id != 0u &&
        ctx->pending_start_payload.resume_token[0] != '\0' &&
        ctx->resume_remote_profile_id[0] != '\0';

    if (chess_discovery_poll(&ctx->discovery, &ctx->discovered_peer)) {
        bool should_select_peer = false;

        chess_lobby_add_or_update_peer(&ctx->lobby, &ctx->discovered_peer.peer, ctx->discovered_peer.tcp_port);

        if (has_resume_target) {
            should_select_peer = SDL_strncmp(
                ctx->discovered_peer.peer.profile_id,
                ctx->resume_remote_profile_id,
                CHESS_PROFILE_ID_STRING_LEN) == 0;
        } else {
            /* Also re-select the same peer so mDNS-resolved attributes
             * (IP, hostname, profile) update an early HELLO-only peer. */
            should_select_peer = !ctx->network_session.peer_available ||
                (ctx->network_session.remote_peer.uuid[0] != '\0' &&
                 SDL_strncmp(ctx->network_session.remote_peer.uuid,
                             ctx->discovered_peer.peer.uuid,
                             CHESS_UUID_STRING_LEN) == 0);
        }

        if (should_select_peer) {
            int peer_idx = -1;
            (void)chess_lobby_find_peer(&ctx->lobby, &ctx->discovered_peer.peer, &peer_idx);
            if (peer_idx >= 0) {
                ctx->lobby.selected_peer_idx = peer_idx;
            }

            chess_network_session_set_remote(&ctx->network_session, &ctx->discovered_peer.peer);
            if (has_resume_target) {
                SDL_Log(
                    "LOBBY: matched persisted resume peer %.8s... (profile %.8s...)",
                    ctx->discovered_peer.peer.uuid,
                    ctx->discovered_peer.peer.profile_id);
                app_set_status_message(ctx, "Adversaire retrouve, tentative de reprise...", 3000u);
            }
        }

        SDL_Log(
            "LOBBY: discovered peer %.8s... (port=%u)",
            ctx->discovered_peer.peer.uuid,
            (unsigned int)ctx->discovered_peer.tcp_port
        );
    }
}

int app_run(void)
{
    AppLoopContext ctx;

    app_loop_context_init_defaults(&ctx);

    if (!app_init_window_and_renderer(&ctx)) {
        return 1;
    }

    if (!app_init_networking(&ctx)) {
        app_loop_context_shutdown(&ctx);
        return 1;
    }

    while (ctx.running) {
        chess_input_handle_events(&ctx);
        app_poll_discovery_and_update_lobby(&ctx);
        chess_net_tick(&ctx);

        chess_network_session_step(&ctx.network_session);
        chess_ui_update_remote_move_animation(&ctx);

        chess_ui_render_frame(&ctx);
    }

    app_loop_context_shutdown(&ctx);
    return 0;
}
