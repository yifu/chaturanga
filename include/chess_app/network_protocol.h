#ifndef CHESS_APP_NETWORK_PROTOCOL_H
#define CHESS_APP_NETWORK_PROTOCOL_H

#include <stdint.h>

#define CHESS_PROTOCOL_VERSION 1u
#define CHESS_DISCOVERY_SERVICE "_chess._tcp.local"

typedef enum ChessMessageType {
    CHESS_MSG_HELLO = 1,
    CHESS_MSG_OFFER,
    CHESS_MSG_ACCEPT,
    CHESS_MSG_START,
    CHESS_MSG_MOVE,
    CHESS_MSG_ACK,
    CHESS_MSG_RESIGN,
    CHESS_MSG_DRAW_OFFER,
    CHESS_MSG_DRAW_ACCEPT,
    CHESS_MSG_HEARTBEAT,
    CHESS_MSG_DISCONNECT
} ChessMessageType;

typedef struct ChessMovePayload {
    uint8_t from_file;
    uint8_t from_rank;
    uint8_t to_file;
    uint8_t to_rank;
    uint8_t promotion;
} ChessMovePayload;

typedef struct ChessPacketHeader {
    uint32_t protocol_version;
    uint32_t message_type;
    uint32_t sequence;
    uint32_t payload_size;
} ChessPacketHeader;

#endif
