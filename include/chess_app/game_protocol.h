#ifndef CHESS_APP_GAME_PROTOCOL_H
#define CHESS_APP_GAME_PROTOCOL_H

#include "chess_app/network_protocol.h"

#include <stdint.h>

typedef struct GameProtocol {
    ChessStartPayload pending_start_payload;
    uint32_t move_sequence;
} GameProtocol;

#endif
