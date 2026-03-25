#ifndef CHESS_APP_APP_CONTEXT_H
#define CHESS_APP_APP_CONTEXT_H

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_STATUS_MESSAGE_LEN 192
#define APP_MOVE_HISTORY_MAX   ((int)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES)
#define APP_MOVE_HISTORY_ENTRY ((int)CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN)

typedef struct AppContext {
    /* ── Window / rendering ─────────────────────────────────────────── */
    int window_size;
    SDL_Window *window;
    SDL_Renderer *renderer;

    /* ── Networking ─────────────────────────────────────────────────── */
    ChessPeerInfo local_peer;
    ChessNetworkSession network_session;
    ChessDiscoveryContext discovery;
    ChessTcpListener listener;
    ChessTcpConnection connection;
    ChessDiscoveredPeer discovered_peer;
    int connect_retry_ms;
    uint64_t next_connect_attempt_at;

    /* ── Game protocol ──────────────────────────────────────────────── */
    ChessStartPayload pending_start_payload;
    uint32_t move_sequence;

    /* ── Resume / persistence ───────────────────────────────────────── */
    bool resume_state_loaded;
    char resume_remote_profile_id[CHESS_PROFILE_ID_STRING_LEN];

    /* ── Game state ─────────────────────────────────────────────────── */
    ChessGameState game_state;
    ChessLobbyState lobby;
    uint16_t move_history_count;
    char move_history[APP_MOVE_HISTORY_MAX][APP_MOVE_HISTORY_ENTRY];

    /* ── UI / input state ───────────────────────────────────────────── */
    bool drag_active;
    ChessPiece drag_piece;
    int drag_from_file;
    int drag_from_rank;
    int drag_mouse_x;
    int drag_mouse_y;
    bool promotion_pending;
    int promotion_to_file;
    int promotion_to_rank;

    /* ── Remote move animation ──────────────────────────────────────── */
    bool remote_move_anim_active;
    ChessPiece remote_move_anim_piece;
    int remote_move_from_file;
    int remote_move_from_rank;
    int remote_move_to_file;
    int remote_move_to_rank;
    uint64_t remote_move_anim_started_at_ms;
    uint32_t remote_move_anim_duration_ms;

    /* ── Status messages ────────────────────────────────────────────── */
    char status_message[APP_STATUS_MESSAGE_LEN];
    uint64_t status_message_until_ms;

    /* ── Cursor ──────────────────────────────────────────────────────── */
    SDL_Cursor *cursor_default;
    SDL_Cursor *cursor_pointer;

    /* ── Run control ────────────────────────────────────────────────── */
    bool running;
} AppContext;

/* Temporary alias so existing app.c code compiles during migration */
typedef AppContext AppLoopContext;

/* ── Functions used by extracted modules (net_handler, etc.) ─────────── */
void app_set_status_message(AppContext *ctx, const char *message, uint32_t duration_ms);
void app_append_move_history(AppContext *ctx, const char *notation);
void app_handle_peer_disconnect(AppContext *ctx, const char *reason);
void app_return_to_lobby(AppContext *ctx);

#endif
