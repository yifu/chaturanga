#include "chess_app/game_state.h"

#include <string.h>

static int abs_i(int v)
{
    return (v < 0) ? -v : v;
}

static bool in_bounds(int file, int rank)
{
    return file >= 0 && file < CHESS_BOARD_SIZE && rank >= 0 && rank < CHESS_BOARD_SIZE;
}

static ChessPlayerColor piece_color(ChessPiece piece)
{
    if (piece >= CHESS_PIECE_WHITE_PAWN && piece <= CHESS_PIECE_WHITE_KING) {
        return CHESS_COLOR_WHITE;
    }
    if (piece >= CHESS_PIECE_BLACK_PAWN && piece <= CHESS_PIECE_BLACK_KING) {
        return CHESS_COLOR_BLACK;
    }
    return CHESS_COLOR_UNASSIGNED;
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

static bool path_clear_straight(const ChessGameState *state, int from_file, int from_rank, int to_file, int to_rank)
{
    int step_file;
    int step_rank;
    int file;
    int rank;

    if (!state) {
        return false;
    }

    if (from_file != to_file && from_rank != to_rank) {
        return false;
    }

    step_file = (to_file > from_file) ? 1 : (to_file < from_file ? -1 : 0);
    step_rank = (to_rank > from_rank) ? 1 : (to_rank < from_rank ? -1 : 0);
    file = from_file + step_file;
    rank = from_rank + step_rank;

    while (file != to_file || rank != to_rank) {
        if ((ChessPiece)state->board[rank][file] != CHESS_PIECE_EMPTY) {
            return false;
        }
        file += step_file;
        rank += step_rank;
    }
    return true;
}

static bool path_clear_diagonal(const ChessGameState *state, int from_file, int from_rank, int to_file, int to_rank)
{
    int dist_file;
    int dist_rank;
    int step_file;
    int step_rank;
    int file;
    int rank;

    if (!state) {
        return false;
    }

    dist_file = abs_i(to_file - from_file);
    dist_rank = abs_i(to_rank - from_rank);
    if (dist_file != dist_rank) {
        return false;
    }

    step_file = (to_file > from_file) ? 1 : -1;
    step_rank = (to_rank > from_rank) ? 1 : -1;
    file = from_file + step_file;
    rank = from_rank + step_rank;

    while (file != to_file && rank != to_rank) {
        if ((ChessPiece)state->board[rank][file] != CHESS_PIECE_EMPTY) {
            return false;
        }
        file += step_file;
        rank += step_rank;
    }
    return true;
}

static bool is_pseudo_legal_move(
    const ChessGameState *state,
    ChessPiece piece,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank)
{
    ChessPiece target_piece;
    int df;
    int dr;

    if (!state || !in_bounds(from_file, from_rank) || !in_bounds(to_file, to_rank)) {
        return false;
    }

    if (from_file == to_file && from_rank == to_rank) {
        return false;
    }

    target_piece = (ChessPiece)state->board[to_rank][to_file];
    if (piece_color(target_piece) == piece_color(piece)) {
        return false;
    }

    df = to_file - from_file;
    dr = to_rank - from_rank;

    switch (piece) {
    case CHESS_PIECE_WHITE_PAWN:
    case CHESS_PIECE_BLACK_PAWN: {
        int dir = (piece == CHESS_PIECE_WHITE_PAWN) ? -1 : 1;
        int start_rank = (piece == CHESS_PIECE_WHITE_PAWN) ? 6 : 1;

        if (df == 0) {
            if (dr == dir && target_piece == CHESS_PIECE_EMPTY) {
                return true;
            }
            if (from_rank == start_rank && dr == (2 * dir) && target_piece == CHESS_PIECE_EMPTY) {
                int mid_rank = from_rank + dir;
                return (ChessPiece)state->board[mid_rank][from_file] == CHESS_PIECE_EMPTY;
            }
            return false;
        }

        if (abs_i(df) == 1 && dr == dir) {
            return target_piece != CHESS_PIECE_EMPTY;
        }
        return false;
    }
    case CHESS_PIECE_WHITE_KNIGHT:
    case CHESS_PIECE_BLACK_KNIGHT:
        return (abs_i(df) == 1 && abs_i(dr) == 2) || (abs_i(df) == 2 && abs_i(dr) == 1);
    case CHESS_PIECE_WHITE_BISHOP:
    case CHESS_PIECE_BLACK_BISHOP:
        return path_clear_diagonal(state, from_file, from_rank, to_file, to_rank);
    case CHESS_PIECE_WHITE_ROOK:
    case CHESS_PIECE_BLACK_ROOK:
        return path_clear_straight(state, from_file, from_rank, to_file, to_rank);
    case CHESS_PIECE_WHITE_QUEEN:
    case CHESS_PIECE_BLACK_QUEEN:
        return path_clear_straight(state, from_file, from_rank, to_file, to_rank) ||
               path_clear_diagonal(state, from_file, from_rank, to_file, to_rank);
    case CHESS_PIECE_WHITE_KING:
    case CHESS_PIECE_BLACK_KING:
        return abs_i(df) <= 1 && abs_i(dr) <= 1;
    case CHESS_PIECE_EMPTY:
    case CHESS_PIECE_COUNT:
    default:
        return false;
    }
}

static bool find_king(const ChessGameState *state, ChessPlayerColor color, int *out_file, int *out_rank)
{
    int rank;
    int file;
    ChessPiece expected = (color == CHESS_COLOR_WHITE) ? CHESS_PIECE_WHITE_KING : CHESS_PIECE_BLACK_KING;

    if (!state || !out_file || !out_rank) {
        return false;
    }

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            if ((ChessPiece)state->board[rank][file] == expected) {
                *out_file = file;
                *out_rank = rank;
                return true;
            }
        }
    }
    return false;
}

static bool is_square_attacked(const ChessGameState *state, int target_file, int target_rank, ChessPlayerColor by_color)
{
    int rank;
    int file;

    if (!state || !in_bounds(target_file, target_rank)) {
        return false;
    }

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            ChessPiece piece = (ChessPiece)state->board[rank][file];
            int df = target_file - file;
            int dr = target_rank - rank;

            if (piece == CHESS_PIECE_EMPTY || piece_color(piece) != by_color) {
                continue;
            }

            switch (piece) {
            case CHESS_PIECE_WHITE_PAWN:
                if (dr == -1 && abs_i(df) == 1) {
                    return true;
                }
                break;
            case CHESS_PIECE_BLACK_PAWN:
                if (dr == 1 && abs_i(df) == 1) {
                    return true;
                }
                break;
            case CHESS_PIECE_WHITE_KNIGHT:
            case CHESS_PIECE_BLACK_KNIGHT:
                if ((abs_i(df) == 1 && abs_i(dr) == 2) || (abs_i(df) == 2 && abs_i(dr) == 1)) {
                    return true;
                }
                break;
            case CHESS_PIECE_WHITE_BISHOP:
            case CHESS_PIECE_BLACK_BISHOP:
                if (path_clear_diagonal(state, file, rank, target_file, target_rank)) {
                    return true;
                }
                break;
            case CHESS_PIECE_WHITE_ROOK:
            case CHESS_PIECE_BLACK_ROOK:
                if (path_clear_straight(state, file, rank, target_file, target_rank)) {
                    return true;
                }
                break;
            case CHESS_PIECE_WHITE_QUEEN:
            case CHESS_PIECE_BLACK_QUEEN:
                if (path_clear_straight(state, file, rank, target_file, target_rank) ||
                    path_clear_diagonal(state, file, rank, target_file, target_rank)) {
                    return true;
                }
                break;
            case CHESS_PIECE_WHITE_KING:
            case CHESS_PIECE_BLACK_KING:
                if (abs_i(df) <= 1 && abs_i(dr) <= 1) {
                    return true;
                }
                break;
            case CHESS_PIECE_EMPTY:
            case CHESS_PIECE_COUNT:
            default:
                break;
            }
        }
    }

    return false;
}

static bool is_king_in_check(const ChessGameState *state, ChessPlayerColor color)
{
    int king_file;
    int king_rank;

    if (!state || !find_king(state, color, &king_file, &king_rank)) {
        return false;
    }

    return is_square_attacked(state, king_file, king_rank, opposite_color(color));
}

static bool is_legal_move(
    const ChessGameState *state,
    ChessPlayerColor moving_color,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank)
{
    ChessGameState copy;
    ChessPiece piece;

    if (!state || !in_bounds(from_file, from_rank) || !in_bounds(to_file, to_rank)) {
        return false;
    }

    piece = (ChessPiece)state->board[from_rank][from_file];
    if (piece == CHESS_PIECE_EMPTY || piece_color(piece) != moving_color) {
        return false;
    }

    if (!is_pseudo_legal_move(state, piece, from_file, from_rank, to_file, to_rank)) {
        return false;
    }

    copy = *state;
    copy.board[to_rank][to_file] = copy.board[from_rank][from_file];
    copy.board[from_rank][from_file] = CHESS_PIECE_EMPTY;

    if (piece == CHESS_PIECE_WHITE_PAWN && to_rank == 0) {
        copy.board[to_rank][to_file] = CHESS_PIECE_WHITE_QUEEN;
    } else if (piece == CHESS_PIECE_BLACK_PAWN && to_rank == 7) {
        copy.board[to_rank][to_file] = CHESS_PIECE_BLACK_QUEEN;
    }

    return !is_king_in_check(&copy, moving_color);
}

static bool apply_move(ChessGameState *state, int from_file, int from_rank, int to_file, int to_rank)
{
    uint8_t piece;
    ChessPiece captured;
    bool is_pawn_move;

    if (!state || !in_bounds(from_file, from_rank) || !in_bounds(to_file, to_rank)) {
        return false;
    }

    piece = state->board[from_rank][from_file];
    if (piece == CHESS_PIECE_EMPTY) {
        return false;
    }

    captured = (ChessPiece)state->board[to_rank][to_file];
    is_pawn_move = piece == CHESS_PIECE_WHITE_PAWN || piece == CHESS_PIECE_BLACK_PAWN;

    if (is_pawn_move || captured != CHESS_PIECE_EMPTY) {
        state->halfmove_clock = 0;
    } else {
        state->halfmove_clock += 1u;
    }

    if (state->side_to_move == CHESS_COLOR_BLACK) {
        state->fullmove_number += 1u;
    }

    state->board[to_rank][to_file] = piece;
    state->board[from_rank][from_file] = CHESS_PIECE_EMPTY;

    if (piece == CHESS_PIECE_WHITE_PAWN && to_rank == 0) {
        state->board[to_rank][to_file] = CHESS_PIECE_WHITE_QUEEN;
    } else if (piece == CHESS_PIECE_BLACK_PAWN && to_rank == 7) {
        state->board[to_rank][to_file] = CHESS_PIECE_BLACK_QUEEN;
    }

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
    state->halfmove_clock = 0;
    state->fullmove_number = 1;
    state->selected_file = -1;
    state->selected_rank = -1;

    /* Black back rank */
    state->board[0][0] = CHESS_PIECE_BLACK_ROOK;
    state->board[0][1] = CHESS_PIECE_BLACK_KNIGHT;
    state->board[0][2] = CHESS_PIECE_BLACK_BISHOP;
    state->board[0][3] = CHESS_PIECE_BLACK_QUEEN;
    state->board[0][4] = CHESS_PIECE_BLACK_KING;
    state->board[0][5] = CHESS_PIECE_BLACK_BISHOP;
    state->board[0][6] = CHESS_PIECE_BLACK_KNIGHT;
    state->board[0][7] = CHESS_PIECE_BLACK_ROOK;
    for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
        state->board[1][file] = CHESS_PIECE_BLACK_PAWN;
    }
    /* White back rank */
    for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
        state->board[6][file] = CHESS_PIECE_WHITE_PAWN;
    }
    state->board[7][0] = CHESS_PIECE_WHITE_ROOK;
    state->board[7][1] = CHESS_PIECE_WHITE_KNIGHT;
    state->board[7][2] = CHESS_PIECE_WHITE_BISHOP;
    state->board[7][3] = CHESS_PIECE_WHITE_QUEEN;
    state->board[7][4] = CHESS_PIECE_WHITE_KING;
    state->board[7][5] = CHESS_PIECE_WHITE_BISHOP;
    state->board[7][6] = CHESS_PIECE_WHITE_KNIGHT;
    state->board[7][7] = CHESS_PIECE_WHITE_ROOK;
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

bool chess_game_select_local_piece(ChessGameState *state, ChessPlayerColor local_color, int file, int rank)
{
    ChessPiece piece;

    if (!state || !in_bounds(file, rank)) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    piece = (ChessPiece)state->board[rank][file];
    if (piece_color(piece) != local_color) {
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
    if (!state || !out_move || !in_bounds(to_file, to_rank) || !state->has_selection) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    if (piece_color((ChessPiece)state->board[state->selected_rank][state->selected_file]) != local_color) {
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

    if (!is_legal_move(
            state,
            local_color,
            state->selected_file,
            state->selected_rank,
            to_file,
            to_rank)) {
        return false;
    }

    return apply_move(state, state->selected_file, state->selected_rank, to_file, to_rank);
}

bool chess_game_apply_remote_move(ChessGameState *state, ChessPlayerColor remote_color, const ChessMovePayload *move)
{
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

    if (piece_color((ChessPiece)state->board[move->from_rank][move->from_file]) != remote_color) {
        return false;
    }

    if (!is_legal_move(
            state,
            remote_color,
            (int)move->from_file,
            (int)move->from_rank,
            (int)move->to_file,
            (int)move->to_rank)) {
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
