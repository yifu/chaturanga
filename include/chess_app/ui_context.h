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
    bool was_already_selected; /* piece was selected before this drag started */
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
    /* Deferred capture: queued while a remote-move animation is in flight */
    bool pending;
} CaptureAnimation;

typedef struct SnapBackAnimation {
    bool active;
    ChessPiece piece;
    int to_file;
    int to_rank;
    float from_x;
    float from_y;
    uint64_t started_at_ms;
    uint32_t duration_ms;
} SnapBackAnimation;

typedef struct UIContext {
    DragState drag;
    RemoteMoveAnimation remote_move_anim;
    CaptureAnimation capture_anim;
    SnapBackAnimation snap_back_anim;
    char status_message[APP_STATUS_MESSAGE_LEN];
    uint64_t status_message_until_ms;
    int history_scroll_offset; /* turn-lines from bottom; 0 = follow latest */
} UIContext;

#endif
