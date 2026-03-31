#ifndef CHESS_APP_GAME_CONTEXT_H
#define CHESS_APP_GAME_CONTEXT_H

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_protocol.h"

#include <stdint.h>

#define APP_MOVE_HISTORY_MAX   ((int)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES)
#define APP_MOVE_HISTORY_ENTRY ((int)CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN)

typedef struct GameContext {
    ChessGameState game_state;
    ChessLobbyState lobby;
    uint16_t move_history_count;
    char move_history[APP_MOVE_HISTORY_MAX][APP_MOVE_HISTORY_ENTRY];
} GameContext;

#endif
