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

#include "embedded_pieces.h"
#include <SDL3_image/SDL_image.h>

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/tcp_transport.h"

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

        SDL_Log("NET: local peer initialized (profile_id=%s display=%s@%s)",
            local_peer->profile_id,
            local_peer->username,
            local_peer->hostname);
    return true;
}

/* AppContext is now defined in chess_app/app_context.h */
/* Persistence functions moved to persistence.c */
/* Lobby rendering moved to ui/lobby.c */
/* Game UI rendering moved to ui/game.c */

/* App context and startup helpers */
static void app_loop_context_init_defaults(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->win.window_size = 640;
    ctx->network.connect_retry_ms = 1000;
    tcp_transport_init(&ctx->network.transport);
    ctx->running = true;
}

static void app_loop_context_shutdown(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    chess_discovery_stop(&ctx->network.discovery);
    chess_lobby_close_all_challenge_connections(&ctx->game.lobby);
    transport_close(&ctx->network.transport.base);
    chess_tcp_listener_close(&ctx->network.listener);
    destroy_piece_textures();
    if (ctx->win.cursor_pointer) {
        SDL_DestroyCursor(ctx->win.cursor_pointer);
    }
    if (ctx->win.cursor_default) {
        SDL_DestroyCursor(ctx->win.cursor_default);
    }
    if (ctx->win.renderer) {
        SDL_DestroyRenderer(ctx->win.renderer);
    }
    if (ctx->win.window) {
        SDL_DestroyWindow(ctx->win.window);
    }
    SDL_Quit();
}

static bool app_init_window_and_renderer(AppContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("APP: SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    ctx->win.window = SDL_CreateWindow(
        "SDL3 Chess Board",
        ctx->win.window_size + CHESS_UI_HISTORY_PANEL_WIDTH,
        ctx->win.window_size,
        0);
    if (!ctx->win.window) {
        SDL_Log("APP: SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    ctx->win.renderer = SDL_CreateRenderer(ctx->win.window, NULL);
    if (!ctx->win.renderer) {
        SDL_Log("APP: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->win.window);
        ctx->win.window = NULL;
        SDL_Quit();
        return false;
    }

    if (!SDL_SetRenderVSync(ctx->win.renderer, 1)) {
        SDL_Log("APP: SDL_SetRenderVSync failed: %s (continuing without vsync)", SDL_GetError());
    }

    init_piece_textures(ctx->win.renderer);

    /* Set window icon to white knight */
    {
        SDL_IOStream *io = SDL_IOFromConstMem(embedded_Chess_nlt60, embedded_Chess_nlt60_size);
        if (io) {
            SDL_Surface *icon = IMG_Load_IO(io, true);
            if (icon) {
                SDL_SetWindowIcon(ctx->win.window, icon);
                SDL_DestroySurface(icon);
            }
        }
    }

    ctx->win.cursor_default = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    ctx->win.cursor_pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    return true;
}

static void app_init_runtime_state(AppContext *ctx)
{
    if (!ctx) {
        return;
    }

    tcp_transport_init(&ctx->network.transport);
    memset(&ctx->network.discovered_peer, 0, sizeof(ctx->network.discovered_peer));
    ctx->network.network_session.start_completed = false;
    memset(&ctx->protocol.pending_start_payload, 0, sizeof(ctx->protocol.pending_start_payload));
    ctx->network.network_session.start_failures = 0u;
    ctx->protocol.move_sequence = 3u;
    ctx->network.next_connect_attempt_at = 0;
    ctx->resume.resume_state_loaded = false;
    ctx->network.network_session.pending_resume_state_sync = false;
    ctx->resume.resume_remote_profile_id[0] = '\0';
    ctx->ui.drag.drag_active = false;
    ctx->ui.drag.drag_piece = CHESS_PIECE_EMPTY;
    ctx->ui.drag.drag_from_file = -1;
    ctx->ui.drag.drag_from_rank = -1;
    ctx->ui.drag.drag_mouse_x = 0;
    ctx->ui.drag.drag_mouse_y = 0;
    ctx->ui.drag.promotion_pending = false;
    ctx->ui.drag.promotion_to_file = -1;
    ctx->ui.drag.promotion_to_rank = -1;
    ctx->ui.remote_move_anim.active = false;
    ctx->ui.remote_move_anim.piece = CHESS_PIECE_EMPTY;
    ctx->ui.remote_move_anim.from_file = -1;
    ctx->ui.remote_move_anim.from_rank = -1;
    ctx->ui.remote_move_anim.to_file = -1;
    ctx->ui.remote_move_anim.to_rank = -1;
    ctx->ui.remote_move_anim.started_at_ms = 0;
    ctx->ui.remote_move_anim.duration_ms = CHESS_REMOTE_MOVE_ANIM_DEFAULT_MS;
    ctx->ui.status_message[0] = '\0';
    ctx->ui.status_message_until_ms = 0;
    ctx->game.move_history_count = 0;

    chess_net_reset_transport_progress(ctx);
    chess_game_state_init(&ctx->game.game_state);
    chess_lobby_init(&ctx->game.lobby);
}

void app_set_status_message(AppContext *ctx, const char *message, uint32_t duration_ms)
{
    if (!ctx || !message) {
        return;
    }

    SDL_strlcpy(ctx->ui.status_message, message, sizeof(ctx->ui.status_message));
    ctx->ui.status_message_until_ms = SDL_GetTicks() + (uint64_t)duration_ms;
}

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

static bool app_init_networking(AppContext *ctx)
{
    if (!ctx) {
        return false;
    }

    if (!init_local_peer(&ctx->network.local_peer)) {
        return false;
    }

    if (!chess_tcp_listener_open(&ctx->network.listener, 0)) {
        SDL_Log("NET: could not create TCP listener on ephemeral port");
        return false;
    }

    SDL_Log("NET: listener ready on port %u", (unsigned int)ctx->network.listener.port);

    app_init_runtime_state(ctx);

    if (chess_persist_load_client_resume_state(ctx)) {
        SDL_Log(
            "NET: loaded persisted resume context (game_id=%u, remote_profile=%.8s...)",
            ctx->protocol.pending_start_payload.game_id,
            ctx->resume.resume_remote_profile_id);
        app_set_status_message(ctx, "Reprise detectee: recherche de l'adversaire...", 3000u);
    }

    if (!chess_discovery_start(&ctx->network.discovery, &ctx->network.local_peer, ctx->network.listener.port)) {
        SDL_Log("NET: discovery start failed");
        return false;
    }

    chess_network_session_init(&ctx->network.network_session, &ctx->network.local_peer);
    return true;
}

/* App frame helpers */
static void app_poll_discovery_and_update_lobby(AppContext *ctx)
{
    bool has_resume_target;

    if (!ctx) {
        return;
    }

    has_resume_target =
        ctx->resume.resume_state_loaded &&
        ctx->protocol.pending_start_payload.game_id != 0u &&
        ctx->protocol.pending_start_payload.resume_token[0] != '\0' &&
        ctx->resume.resume_remote_profile_id[0] != '\0';

    /* Process service removals first so re-registered peers are handled
     * in the correct order (remove stale, then add fresh). */
    {
        char removed_id[CHESS_PROFILE_ID_STRING_LEN];
        while (chess_discovery_poll_removal(&ctx->network.discovery, removed_id, sizeof(removed_id))) {
            if (chess_lobby_remove_peer_by_profile_id(&ctx->game.lobby, removed_id)) {
                SDL_Log("LOBBY: removed departed peer %.8s...", removed_id);
            }
        }
    }

    if (chess_discovery_check_result(&ctx->network.discovery, &ctx->network.discovered_peer)) {
        bool should_select_peer = false;

        chess_lobby_add_or_update_peer(&ctx->game.lobby, &ctx->network.discovered_peer.peer, ctx->network.discovered_peer.tcp_ipv4, ctx->network.discovered_peer.tcp_port);

        /* Apply a buffered OFFER that arrived before mDNS discovery. */
        if (ctx->network.network_session.pending_incoming_offer &&
            SDL_strncmp(ctx->network.discovered_peer.peer.profile_id,
                        ctx->network.network_session.pending_offer_profile_id,
                        CHESS_PROFILE_ID_STRING_LEN) == 0) {
            int offer_peer_idx = -1;
            (void)chess_lobby_find_peer(&ctx->game.lobby, &ctx->network.discovered_peer.peer, &offer_peer_idx);
            if (offer_peer_idx >= 0) {
                chess_lobby_set_challenge_state(&ctx->game.lobby, offer_peer_idx, CHESS_CHALLENGE_INCOMING_PENDING);
                chess_network_session_set_remote(&ctx->network.network_session, &ctx->game.lobby.discovered_peers[offer_peer_idx].peer);
                SDL_Log("LOBBY: applied buffered OFFER from %.8s...",
                        ctx->network.discovered_peer.peer.profile_id);
            }
            ctx->network.network_session.pending_incoming_offer = false;
        }

        if (has_resume_target) {
            should_select_peer = SDL_strncmp(
                ctx->network.discovered_peer.peer.profile_id,
                ctx->resume.resume_remote_profile_id,
                CHESS_PROFILE_ID_STRING_LEN) == 0;
        } else if (ctx->network.network_session.peer_available &&
                   ctx->network.network_session.remote_peer.profile_id[0] != '\0') {
            /* Re-select the same peer so mDNS-resolved attributes
             * (IP, hostname, profile) update an early HELLO-only peer.
             * Do NOT auto-select when no peer is set yet — the user
             * must click in the lobby to initiate a connection. */
            should_select_peer =
                SDL_strncmp(ctx->network.network_session.remote_peer.profile_id,
                            ctx->network.discovered_peer.peer.profile_id,
                            CHESS_PROFILE_ID_STRING_LEN) == 0;
        }

        if (should_select_peer) {
            int peer_idx = -1;
            (void)chess_lobby_find_peer(&ctx->game.lobby, &ctx->network.discovered_peer.peer, &peer_idx);
            if (peer_idx >= 0) {
                ctx->game.lobby.selected_peer_idx = peer_idx;
            }

            chess_network_session_set_remote(&ctx->network.network_session, &ctx->network.discovered_peer.peer);
            if (has_resume_target) {
                ctx->network.network_session.role = CHESS_ROLE_CLIENT;
                SDL_Log(
                    "LOBBY: matched persisted resume peer %.8s...",
                    ctx->network.discovered_peer.peer.profile_id);
                app_set_status_message(ctx, "Adversaire retrouve, tentative de reprise...", 3000u);
            }
        }

        SDL_Log(
            "LOBBY: discovered peer %.8s... (port=%u)",
            ctx->network.discovered_peer.peer.profile_id,
            (unsigned int)ctx->network.discovered_peer.tcp_port
        );
    }
}

int app_run(void)
{
    AppContext ctx;

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
        chess_net_tick(&ctx);
        app_poll_discovery_and_update_lobby(&ctx);

        chess_ui_update_remote_move_animation(&ctx);
        chess_ui_update_capture_animation(&ctx);

        chess_ui_render_frame(&ctx);
    }

    app_loop_context_shutdown(&ctx);
    return 0;
}
