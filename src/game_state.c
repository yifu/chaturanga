#include "chess_app/game_state.h"

#include <string.h>

static bool in_bounds(int file, int rank)
{
    return file >= 0 && file < CHESS_BOARD_SIZE && rank >= 0 && rank < CHESS_BOARD_SIZE;
}

static ChessPiece color_to_pawn(ChessPlayerColor color)
{
    if (color == CHESS_COLOR_WHITE) {
        return CHESS_PIECE_WHITE_PAWN;
    }
    if (color == CHESS_COLOR_BLACK) {
        return CHESS_PIECE_BLACK_PAWN;
    }
    return CHESS_PIECE_EMPTY;
}

static ChessPlayerColor opposite_color(ChessPlayerColor color)
{
    if (color == CHESS_COLOR_WHITE) {
        return CHESS_COLOR_BLACK;
    }
    if (color == CHESS_COLOR_BLACK) {
        return CHESS_COLOR_WHITE;
    }
    return CHESS_COLOR_UNASSIGNED;
}

static bool apply_move(ChessGameState *state, int from_file, int from_rank, int to_file, int to_rank)
{
    uint8_t piece;

    if (!state || !in_bounds(from_file, from_rank) || !in_bounds(to_file, to_rank)) {
        return false;
    }

    piece = state->board[from_rank][from_file];
    if (piece == CHESS_PIECE_EMPTY) {
        return false;
    }

    state->board[to_rank][to_file] = piece;
    state->board[from_rank][from_file] = CHESS_PIECE_EMPTY;
    state->side_to_move = opposite_color(state->side_to_move);
    state->has_selection = false;
    state->selected_file = -1;
    state->selected_rank = -1;
    return true;
}

void chess_game_state_init(ChessGameState *state)
{
    int file;

    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->side_to_move = CHESS_COLOR_WHITE;
    state->selected_file = -1;
    state->selected_rank = -1;

    for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
        state->board[1][file] = CHESS_PIECE_BLACK_PAWN;
        state->board[6][file] = CHESS_PIECE_WHITE_PAWN;
    }
}

void chess_game_clear_selection(ChessGameState *state)
{
    if (!state) {
        return;
    }

    state->has_selection = false;
    state->selected_file = -1;
    state->selected_rank = -1;
}

ChessPiece chess_game_get_piece(const ChessGameState *state, int file, int rank)
{
    if (!state || !in_bounds(file, rank)) {
        return CHESS_PIECE_EMPTY;
    }

    return (ChessPiece)state->board[rank][file];
}

bool chess_game_select_local_pawn(ChessGameState *state, ChessPlayerColor local_color, int file, int rank)
{
    ChessPiece expected;

    if (!state || !in_bounds(file, rank)) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    expected = color_to_pawn(local_color);
    if ((ChessPiece)state->board[rank][file] != expected) {
        return false;
    }

    state->has_selection = true;
    state->selected_file = file;
    state->selected_rank = rank;
    return true;
}

bool chess_game_try_local_move(
    ChessGameState *state,
    ChessPlayerColor local_color,
    int to_file,
    int to_rank,
    ChessMovePayload *out_move)
{
    ChessPiece expected;

    if (!state || !out_move || !in_bounds(to_file, to_rank) || !state->has_selection) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    expected = color_to_pawn(local_color);
    if ((ChessPiece)state->board[state->selected_rank][state->selected_file] != expected) {
        return false;
    }

    if (state->selected_file == to_file && state->selected_rank == to_rank) {
        chess_game_clear_selection(state);
        return false;
    }

    out_move->from_file = (uint8_t)state->selected_file;
    out_move->from_rank = (uint8_t)state->selected_rank;
    out_move->to_file = (uint8_t)to_file;
    out_move->to_rank = (uint8_t)to_rank;
    out_move->promotion = 0u;

    return apply_move(state, state->selected_file, state->selected_rank, to_file, to_rank);
}

bool chess_game_apply_remote_move(ChessGameState *state, ChessPlayerColor remote_color, const ChessMovePayload *move)
{
    ChessPiece expected;

    if (!state || !move) {
        return false;
    }

    if (!in_bounds((int)move->from_file, (int)move->from_rank) ||
        !in_bounds((int)move->to_file, (int)move->to_rank)) {
        return false;
    }

    if (state->side_to_move != remote_color) {
        return false;
    }

    expected = color_to_pawn(remote_color);
    if ((ChessPiece)state->board[move->from_rank][move->from_file] != expected) {
        return false;
    }

    return apply_move(
        state,
        (int)move->from_file,
        (int)move->from_rank,
        (int)move->to_file,
        (int)move->to_rank
    );
}
