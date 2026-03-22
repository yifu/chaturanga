#ifndef CHESS_APP_GAME_STATE_H
#define CHESS_APP_GAME_STATE_H

#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#define CHESS_BOARD_SIZE 8

typedef enum ChessPiece {
    CHESS_PIECE_EMPTY = 0,
    CHESS_PIECE_WHITE_PAWN,
    CHESS_PIECE_BLACK_PAWN
} ChessPiece;

typedef struct ChessGameState {
    uint8_t board[CHESS_BOARD_SIZE][CHESS_BOARD_SIZE]; /* [rank][file] */
    ChessPlayerColor side_to_move;
    bool has_selection;
    int selected_file;
    int selected_rank;
} ChessGameState;

void chess_game_state_init(ChessGameState *state);
void chess_game_clear_selection(ChessGameState *state);
ChessPiece chess_game_get_piece(const ChessGameState *state, int file, int rank);
bool chess_game_select_local_pawn(ChessGameState *state, ChessPlayerColor local_color, int file, int rank);
bool chess_game_try_local_move(
    ChessGameState *state,
    ChessPlayerColor local_color,
    int to_file,
    int to_rank,
    ChessMovePayload *out_move
);
bool chess_game_apply_remote_move(ChessGameState *state, ChessPlayerColor remote_color, const ChessMovePayload *move);

#endif
