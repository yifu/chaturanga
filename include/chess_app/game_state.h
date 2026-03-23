#ifndef CHESS_APP_GAME_STATE_H
#define CHESS_APP_GAME_STATE_H

#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#define CHESS_BOARD_SIZE 8

typedef enum ChessPiece {
    CHESS_PIECE_EMPTY = 0,
    CHESS_PIECE_WHITE_PAWN,
    CHESS_PIECE_WHITE_KNIGHT,
    CHESS_PIECE_WHITE_BISHOP,
    CHESS_PIECE_WHITE_ROOK,
    CHESS_PIECE_WHITE_QUEEN,
    CHESS_PIECE_WHITE_KING,
    CHESS_PIECE_BLACK_PAWN,
    CHESS_PIECE_BLACK_KNIGHT,
    CHESS_PIECE_BLACK_BISHOP,
    CHESS_PIECE_BLACK_ROOK,
    CHESS_PIECE_BLACK_QUEEN,
    CHESS_PIECE_BLACK_KING,
    CHESS_PIECE_COUNT
} ChessPiece;

typedef enum ChessGameOutcome {
    CHESS_OUTCOME_NONE = 0,
    CHESS_OUTCOME_CHECKMATE_WHITE_WINS,
    CHESS_OUTCOME_CHECKMATE_BLACK_WINS,
    CHESS_OUTCOME_STALEMATE,
    CHESS_OUTCOME_FIFTY_MOVE_RULE
} ChessGameOutcome;

typedef struct ChessGameState {
    uint8_t board[CHESS_BOARD_SIZE][CHESS_BOARD_SIZE]; /* [rank][file] */
    ChessPlayerColor side_to_move;
    uint16_t halfmove_clock;
    uint16_t fullmove_number;
    bool white_can_castle_kingside;
    bool white_can_castle_queenside;
    bool black_can_castle_kingside;
    bool black_can_castle_queenside;
    int8_t en_passant_target_file;
    int8_t en_passant_target_rank;
    bool has_selection;
    int selected_file;
    int selected_rank;
    ChessGameOutcome outcome;
} ChessGameState;

void chess_game_state_init(ChessGameState *state);
void chess_game_clear_selection(ChessGameState *state);
ChessPiece chess_game_get_piece(const ChessGameState *state, int file, int rank);
bool chess_game_select_local_piece(ChessGameState *state, ChessPlayerColor local_color, int file, int rank);
bool chess_game_local_move_requires_promotion(
    const ChessGameState *state,
    ChessPlayerColor local_color,
    int to_file,
    int to_rank
);
bool chess_game_try_local_move(
    ChessGameState *state,
    ChessPlayerColor local_color,
    int to_file,
    int to_rank,
    uint8_t promotion,
    ChessMovePayload *out_move
);
bool chess_game_apply_remote_move(ChessGameState *state, ChessPlayerColor remote_color, const ChessMovePayload *move);

#endif
