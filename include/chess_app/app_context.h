#ifndef CHESS_APP_APP_CONTEXT_H
#define CHESS_APP_APP_CONTEXT_H

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/tcp_transport.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_STATUS_MESSAGE_LEN 192
#define APP_MOVE_HISTORY_MAX   ((int)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES)
#define APP_MOVE_HISTORY_ENTRY ((int)CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN)

/* ── Window / rendering ──────────────────────────────────────────────── */
typedef struct AppWindow {
    int window_size;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Cursor *cursor_default;
    SDL_Cursor *cursor_pointer;
} AppWindow;

/* ── Networking ──────────────────────────────────────────────────────── */
typedef struct NetworkContext {
    ChessPeerInfo local_peer;
    ChessNetworkSession network_session;
    ChessDiscoveryContext discovery;
    ChessTcpListener listener;
    TcpTransport transport;              /* replaces: connection + recv_buffer */
    ChessDiscoveredPeer discovered_peer;
    int connect_retry_ms;
    uint64_t next_connect_attempt_at;
} NetworkContext;

/* ── Game protocol ───────────────────────────────────────────────────── */
typedef struct GameProtocol {
    ChessStartPayload pending_start_payload;
    uint32_t move_sequence;
} GameProtocol;

/* ── Resume / persistence ────────────────────────────────────────────── */
typedef struct ResumeContext {
    bool resume_state_loaded;
    char resume_remote_profile_id[CHESS_PROFILE_ID_STRING_LEN];
} ResumeContext;

/* ── Game state ──────────────────────────────────────────────────────── */
typedef struct GameContext {
    ChessGameState game_state;
    ChessLobbyState lobby;
    uint16_t move_history_count;
    char move_history[APP_MOVE_HISTORY_MAX][APP_MOVE_HISTORY_ENTRY];
} GameContext;

/* ── Drag / input state ──────────────────────────────────────────────── */
typedef struct DragState {
    bool drag_active;
    ChessPiece drag_piece;
    int drag_from_file;
    int drag_from_rank;
    int drag_mouse_x;
    int drag_mouse_y;
    bool promotion_pending;
    int promotion_to_file;
    int promotion_to_rank;
} DragState;

/* ── Remote move animation ───────────────────────────────────────────── */
typedef struct RemoteMoveAnimation {
    bool active;
    ChessPiece piece;
    int from_file;
    int from_rank;
    int to_file;
    int to_rank;
    uint64_t started_at_ms;
    uint32_t duration_ms;
} RemoteMoveAnimation;

/* ── Capture animation (piece flies to panel, shrinking) ─────────────── */
typedef struct CaptureAnimation {
    bool active;
    ChessPiece piece;
    int from_file;
    int from_rank;
    bool target_top;  /* true = fly to top panel */
    uint64_t started_at_ms;
    uint32_t duration_ms;
} CaptureAnimation;

/* ── UI context ──────────────────────────────────────────────────────── */
typedef struct UIContext {
    DragState drag;
    RemoteMoveAnimation remote_move_anim;
    CaptureAnimation capture_anim;
    char status_message[APP_STATUS_MESSAGE_LEN];
    uint64_t status_message_until_ms;
} UIContext;

typedef struct AppContext {
    AppWindow win;
    NetworkContext network;
    GameProtocol protocol;
    ResumeContext resume;
    GameContext game;
    UIContext ui;
    bool running;
} AppContext;

/* ── Functions used by extracted modules (net_handler, etc.) ─────────── */
void app_set_status_message(AppContext *ctx, const char *message, uint32_t duration_ms);
void app_append_move_history(AppContext *ctx, const char *notation);
void app_handle_peer_disconnect(AppContext *ctx, const char *reason);
void app_return_to_lobby(AppContext *ctx);

#endif
