#ifndef CHESS_APP_GAME_STATE_H
#define CHESS_APP_GAME_STATE_H

#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stddef.h>
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
    CHESS_OUTCOME_FIFTY_MOVE_RULE,
    CHESS_OUTCOME_WHITE_RESIGNED,
    CHESS_OUTCOME_BLACK_RESIGNED,
    CHESS_OUTCOME_DRAW_AGREED
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
    uint8_t captured[CHESS_PIECE_COUNT];
    bool has_last_move;
    int8_t last_move_from_file;
    int8_t last_move_from_rank;
    int8_t last_move_to_file;
    int8_t last_move_to_rank;
} ChessGameState;

typedef struct ChessCapturedPieces {
    uint8_t count[CHESS_PIECE_COUNT];
} ChessCapturedPieces;

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
bool chess_move_format_algebraic_notation(
    const ChessGameState *state,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank,
    uint8_t promotion,
    char *out,
    size_t out_size
);
bool chess_game_apply_remote_move(ChessGameState *state, ChessPlayerColor remote_color, const ChessMovePayload *move);
void chess_game_compute_captured(const ChessGameState *state, ChessCapturedPieces *out);

#endif
