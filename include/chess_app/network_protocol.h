#ifndef CHESS_APP_NETWORK_PROTOCOL_H
#define CHESS_APP_NETWORK_PROTOCOL_H

#include <stdint.h>

#include "chess_app/network_peer.h"

#define CHESS_PROTOCOL_VERSION 1u
#define CHESS_DISCOVERY_SERVICE "_chess._tcp.local"
#define CHESS_PROTOCOL_SNAPSHOT_BOARD_CELLS 64u
#define CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES 300u
#define CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN 24u
#define CHESS_PIECE_COUNT_PROTOCOL 13u

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
    CHESS_MSG_DRAW_DECLINE,
    CHESS_MSG_RESUME_REQUEST,
    CHESS_MSG_RESUME_RESPONSE,
    CHESS_MSG_STATE_SNAPSHOT,
    CHESS_MSG_HEARTBEAT,
    CHESS_MSG_DISCONNECT
} ChessMessageType;

typedef enum ChessResumeStatus {
    CHESS_RESUME_REJECTED = 0,
    CHESS_RESUME_ACCEPTED = 1
} ChessResumeStatus;

typedef enum ChessPlayerColor {
    CHESS_COLOR_UNASSIGNED = 0,
    CHESS_COLOR_WHITE = 1,
    CHESS_COLOR_BLACK = 2
} ChessPlayerColor;

typedef enum ChessPromotionType {
    CHESS_PROMOTION_NONE = 0,
    CHESS_PROMOTION_QUEEN = 1,
    CHESS_PROMOTION_ROOK = 2,
    CHESS_PROMOTION_BISHOP = 3,
    CHESS_PROMOTION_KNIGHT = 4
} ChessPromotionType;

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
    char profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char username[CHESS_PEER_USERNAME_MAX_LEN];
    char hostname[CHESS_PEER_HOSTNAME_MAX_LEN];
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
    char white_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char black_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char resume_token[CHESS_UUID_STRING_LEN];
} ChessStartPayload;

typedef struct ChessResumeRequestPayload {
    uint32_t game_id;
    char profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char resume_token[CHESS_UUID_STRING_LEN];
} ChessResumeRequestPayload;

typedef struct ChessResumeResponsePayload {
    uint32_t status;
    uint32_t game_id;
} ChessResumeResponsePayload;

typedef struct ChessStateSnapshotPayload {
    uint32_t game_id;
    uint32_t side_to_move;
    uint32_t fullmove_number;
    uint32_t halfmove_clock;
    uint32_t outcome;
    uint8_t white_can_castle_kingside;
    uint8_t white_can_castle_queenside;
    uint8_t black_can_castle_kingside;
    uint8_t black_can_castle_queenside;
    int8_t en_passant_target_file;
    int8_t en_passant_target_rank;
    uint8_t _padding[2];
    uint16_t move_history_count;
    uint8_t _padding2[2];
    char resume_token[CHESS_UUID_STRING_LEN];
    uint8_t board[CHESS_PROTOCOL_SNAPSHOT_BOARD_CELLS];
    uint8_t captured[CHESS_PIECE_COUNT_PROTOCOL];
    uint8_t _padding3[3];
    char move_history[CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES][CHESS_PROTOCOL_MOVE_HISTORY_ENTRY_LEN];
} ChessStateSnapshotPayload;

typedef struct ChessHeartbeatPayload {
    uint32_t tick;
} ChessHeartbeatPayload;

typedef struct ChessOfferPayload {
    char challenger_profile_id[CHESS_PROFILE_ID_STRING_LEN];
} ChessOfferPayload;

typedef struct ChessAcceptPayload {
    char acceptor_profile_id[CHESS_PROFILE_ID_STRING_LEN];
} ChessAcceptPayload;

#endif
