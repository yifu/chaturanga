#ifndef CHESS_APP_APP_CONTEXT_H
#define CHESS_APP_APP_CONTEXT_H

#include "chess_app/app_window.h"
#include "chess_app/game_context.h"
#include "chess_app/game_protocol.h"
#include "chess_app/network_context.h"
#include "chess_app/resume_context.h"
#include "chess_app/ui_context.h"

#include <stdbool.h>

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
