#ifndef CHESS_APP_UI_CONTEXT_H
#define CHESS_APP_UI_CONTEXT_H

#include "chess_app/game_state.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_STATUS_MESSAGE_LEN 192

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

typedef struct CaptureAnimation {
    bool active;
    ChessPiece piece;
    int from_file;
    int from_rank;
    bool target_top;
    uint64_t started_at_ms;
    uint32_t duration_ms;
} CaptureAnimation;

typedef struct UIContext {
    DragState drag;
    RemoteMoveAnimation remote_move_anim;
    CaptureAnimation capture_anim;
    char status_message[APP_STATUS_MESSAGE_LEN];
    uint64_t status_message_until_ms;
} UIContext;

#endif
