#ifndef CHESS_APP_NETWORK_PROTOCOL_H
#define CHESS_APP_NETWORK_PROTOCOL_H

#include <stdint.h>

#include "chess_app/network_peer.h"

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

typedef enum ChessPlayerColor {
    CHESS_COLOR_UNASSIGNED = 0,
    CHESS_COLOR_WHITE = 1,
    CHESS_COLOR_BLACK = 2
} ChessPlayerColor;

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

typedef struct ChessHelloPayload {
    char uuid[CHESS_UUID_STRING_LEN];
    uint32_t role;
} ChessHelloPayload;

typedef struct ChessAckPayload {
    uint32_t acked_message_type;
    uint32_t acked_sequence;
    uint32_t status_code;
} ChessAckPayload;

typedef struct ChessStartPayload {
    uint32_t game_id;
    uint32_t assigned_color;
    uint32_t initial_turn;
    char white_uuid[CHESS_UUID_STRING_LEN];
    char black_uuid[CHESS_UUID_STRING_LEN];
} ChessStartPayload;

typedef struct ChessHeartbeatPayload {
    uint32_t tick;
} ChessHeartbeatPayload;

typedef struct ChessOfferPayload {
    char challenger_uuid[CHESS_UUID_STRING_LEN];
    uint16_t challenger_port;
} ChessOfferPayload;

typedef struct ChessAcceptPayload {
    char acceptor_uuid[CHESS_UUID_STRING_LEN];
    uint16_t acceptor_port;
} ChessAcceptPayload;

#endif
