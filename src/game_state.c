#include "chess_app/game_state.h"
#include "game_state_internal.h"

#include <stdio.h>
#include <string.h>

bool chess_gs_in_bounds(int file, int rank)
{
    return file >= 0 && file < CHESS_BOARD_SIZE && rank >= 0 && rank < CHESS_BOARD_SIZE;
}

ChessPlayerColor chess_gs_piece_color(ChessPiece piece)
{
    if (piece >= CHESS_PIECE_WHITE_PAWN && piece <= CHESS_PIECE_WHITE_KING) {
        return CHESS_COLOR_WHITE;
    }
    if (piece >= CHESS_PIECE_BLACK_PAWN && piece <= CHESS_PIECE_BLACK_KING) {
        return CHESS_COLOR_BLACK;
    }
    return CHESS_COLOR_UNASSIGNED;
}

bool chess_gs_is_castling_king_move(ChessPiece piece, int from_file, int from_rank, int to_file, int to_rank)
{
    if (piece == CHESS_PIECE_WHITE_KING) {
        return from_file == 4 && from_rank == 7 && (to_file == 2 || to_file == 6) && to_rank == 7;
    }
    if (piece == CHESS_PIECE_BLACK_KING) {
        return from_file == 4 && from_rank == 0 && (to_file == 2 || to_file == 6) && to_rank == 0;
    }
    return false;
}

bool chess_gs_is_en_passant_capture_move(
    const ChessGameState *state,
    ChessPiece piece,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank)
{
    int dir;
    ChessPiece adjacent_piece;

    if (!state) {
        return false;
    }

    if (piece != CHESS_PIECE_WHITE_PAWN && piece != CHESS_PIECE_BLACK_PAWN) {
        return false;
    }

    dir = (piece == CHESS_PIECE_WHITE_PAWN) ? -1 : 1;
    if (chess_gs_abs_i(to_file - from_file) != 1 || (to_rank - from_rank) != dir) {
        return false;
    }

    if (state->en_passant_target_file != to_file || state->en_passant_target_rank != to_rank) {
        return false;
    }

    adjacent_piece = (ChessPiece)state->board[from_rank][to_file];
    if (piece == CHESS_PIECE_WHITE_PAWN) {
        return adjacent_piece == CHESS_PIECE_BLACK_PAWN;
    }

    return adjacent_piece == CHESS_PIECE_WHITE_PAWN;
}

bool chess_gs_is_pawn_promotion_move(ChessPiece piece, int to_rank)
{
    return (piece == CHESS_PIECE_WHITE_PAWN && to_rank == 0) ||
           (piece == CHESS_PIECE_BLACK_PAWN && to_rank == 7);
}

bool chess_gs_apply_move(
    ChessGameState *state,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank,
    uint8_t promotion)
{
    uint8_t piece;
    ChessPiece captured;
    bool is_pawn_move;
    bool is_castling;
    bool is_en_passant;

    if (!state || !chess_gs_in_bounds(from_file, from_rank) || !chess_gs_in_bounds(to_file, to_rank)) {
        return false;
    }

    piece = state->board[from_rank][from_file];
    if (piece == CHESS_PIECE_EMPTY) {
        return false;
    }

    captured = (ChessPiece)state->board[to_rank][to_file];
    is_pawn_move = piece == CHESS_PIECE_WHITE_PAWN || piece == CHESS_PIECE_BLACK_PAWN;
    is_castling = chess_gs_is_castling_king_move((ChessPiece)piece, from_file, from_rank, to_file, to_rank);
    is_en_passant = chess_gs_is_en_passant_capture_move(state, (ChessPiece)piece, from_file, from_rank, to_file, to_rank);

    if (captured != CHESS_PIECE_EMPTY && (int)captured < CHESS_PIECE_COUNT) {
        state->captured[(int)captured]++;
    }
    if (is_en_passant) {
        uint8_t ep_victim = state->board[from_rank][to_file];
        if (ep_victim != CHESS_PIECE_EMPTY && (int)ep_victim < CHESS_PIECE_COUNT) {
            state->captured[(int)ep_victim]++;
        }
    }

    if (piece == CHESS_PIECE_WHITE_KING) {
        state->white_can_castle_kingside = false;
        state->white_can_castle_queenside = false;
    } else if (piece == CHESS_PIECE_BLACK_KING) {
        state->black_can_castle_kingside = false;
        state->black_can_castle_queenside = false;
    } else if (piece == CHESS_PIECE_WHITE_ROOK) {
        if (from_file == 0 && from_rank == 7) {
            state->white_can_castle_queenside = false;
        } else if (from_file == 7 && from_rank == 7) {
            state->white_can_castle_kingside = false;
        }
    } else if (piece == CHESS_PIECE_BLACK_ROOK) {
        if (from_file == 0 && from_rank == 0) {
            state->black_can_castle_queenside = false;
        } else if (from_file == 7 && from_rank == 0) {
            state->black_can_castle_kingside = false;
        }
    }

    if (captured == CHESS_PIECE_WHITE_ROOK) {
        if (to_file == 0 && to_rank == 7) {
            state->white_can_castle_queenside = false;
        } else if (to_file == 7 && to_rank == 7) {
            state->white_can_castle_kingside = false;
        }
    } else if (captured == CHESS_PIECE_BLACK_ROOK) {
        if (to_file == 0 && to_rank == 0) {
            state->black_can_castle_queenside = false;
        } else if (to_file == 7 && to_rank == 0) {
            state->black_can_castle_kingside = false;
        }
    }

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

    if (is_en_passant) {
        state->board[from_rank][to_file] = CHESS_PIECE_EMPTY;
    }

    if (is_castling) {
        if (to_file == 6) {
            state->board[to_rank][5] = state->board[to_rank][7];
            state->board[to_rank][7] = CHESS_PIECE_EMPTY;
        } else {
            state->board[to_rank][3] = state->board[to_rank][0];
            state->board[to_rank][0] = CHESS_PIECE_EMPTY;
        }
    }

    if (chess_gs_is_pawn_promotion_move((ChessPiece)piece, to_rank)) {
        ChessPiece promoted = chess_gs_promoted_piece_for_choice(chess_gs_piece_color((ChessPiece)piece), promotion);
        if (promoted == CHESS_PIECE_EMPTY) {
            return false;
        }
        state->board[to_rank][to_file] = (uint8_t)promoted;
    }

    state->en_passant_target_file = -1;
    state->en_passant_target_rank = -1;
    if (piece == CHESS_PIECE_WHITE_PAWN && from_rank == 6 && to_rank == 4) {
        state->en_passant_target_file = (int8_t)from_file;
        state->en_passant_target_rank = 5;
    } else if (piece == CHESS_PIECE_BLACK_PAWN && from_rank == 1 && to_rank == 3) {
        state->en_passant_target_file = (int8_t)from_file;
        state->en_passant_target_rank = 2;
    }

    state->side_to_move = chess_gs_opposite_color(state->side_to_move);
    state->has_selection = false;
    state->selected_file = -1;
    state->selected_rank = -1;

    {
        ChessPlayerColor next_color = state->side_to_move;
        if (state->halfmove_clock >= 100) {
            state->outcome = CHESS_OUTCOME_FIFTY_MOVE_RULE;
        } else if (!chess_gs_has_any_legal_move(state, next_color)) {
            if (chess_gs_is_king_in_check(state, next_color)) {
                state->outcome = (next_color == CHESS_COLOR_WHITE)
                    ? CHESS_OUTCOME_CHECKMATE_BLACK_WINS
                    : CHESS_OUTCOME_CHECKMATE_WHITE_WINS;
            } else {
                state->outcome = CHESS_OUTCOME_STALEMATE;
            }
        }
    }

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
    state->white_can_castle_kingside = true;
    state->white_can_castle_queenside = true;
    state->black_can_castle_kingside = true;
    state->black_can_castle_queenside = true;
    state->en_passant_target_file = -1;
    state->en_passant_target_rank = -1;
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
    if (!state || !chess_gs_in_bounds(file, rank)) {
        return CHESS_PIECE_EMPTY;
    }

    return (ChessPiece)state->board[rank][file];
}

bool chess_game_select_local_piece(ChessGameState *state, ChessPlayerColor local_color, int file, int rank)
{
    ChessPiece piece;

    if (!state || !chess_gs_in_bounds(file, rank)) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    piece = (ChessPiece)state->board[rank][file];
    if (chess_gs_piece_color(piece) != local_color) {
        return false;
    }

    state->has_selection = true;
    state->selected_file = file;
    state->selected_rank = rank;
    return true;
}

bool chess_game_local_move_requires_promotion(
    const ChessGameState *state,
    ChessPlayerColor local_color,
    int to_file,
    int to_rank)
{
    ChessPiece piece;

    if (!state || !chess_gs_in_bounds(to_file, to_rank) || !state->has_selection) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    piece = (ChessPiece)state->board[state->selected_rank][state->selected_file];
    if (chess_gs_piece_color(piece) != local_color) {
        return false;
    }

    if (!chess_gs_is_pawn_promotion_move(piece, to_rank)) {
        return false;
    }

    /* Use any valid promotion choice as a legality probe here. The actual
     * promoted piece is selected later by the UI/network flow. */
    return chess_gs_is_legal_move(
        state,
        local_color,
        state->selected_file,
        state->selected_rank,
        to_file,
        to_rank,
        CHESS_PROMOTION_QUEEN);
}

bool chess_game_try_local_move(
    ChessGameState *state,
    ChessPlayerColor local_color,
    int to_file,
    int to_rank,
    uint8_t promotion,
    ChessMovePayload *out_move)
{
    if (!state || !out_move || !chess_gs_in_bounds(to_file, to_rank) || !state->has_selection) {
        return false;
    }

    if (state->side_to_move != local_color) {
        return false;
    }

    if (chess_gs_piece_color((ChessPiece)state->board[state->selected_rank][state->selected_file]) != local_color) {
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
    out_move->promotion = promotion;

    if (!chess_gs_is_legal_move(
            state,
            local_color,
            state->selected_file,
            state->selected_rank,
            to_file,
            to_rank,
            promotion)) {
        return false;
    }

    return chess_gs_apply_move(state, state->selected_file, state->selected_rank, to_file, to_rank, promotion);
}

bool chess_game_apply_remote_move(ChessGameState *state, ChessPlayerColor remote_color, const ChessMovePayload *move)
{
    if (!state || !move) {
        return false;
    }

    if (!chess_gs_in_bounds((int)move->from_file, (int)move->from_rank) ||
        !chess_gs_in_bounds((int)move->to_file, (int)move->to_rank)) {
        return false;
    }

    if (state->side_to_move != remote_color) {
        return false;
    }

    if (chess_gs_piece_color((ChessPiece)state->board[move->from_rank][move->from_file]) != remote_color) {
        return false;
    }

    if (!chess_gs_is_legal_move(
            state,
            remote_color,
            (int)move->from_file,
            (int)move->from_rank,
            (int)move->to_file,
            (int)move->to_rank,
            move->promotion)) {
        return false;
    }

    return chess_gs_apply_move(
        state,
        (int)move->from_file,
        (int)move->from_rank,
        (int)move->to_file,
        (int)move->to_rank,
        move->promotion
    );
}

void chess_game_compute_captured(const ChessGameState *state, ChessCapturedPieces *out)
{
    memset(out, 0, sizeof(*out));

    if (!state) {
        return;
    }

    memcpy(out->count, state->captured, sizeof(out->count));
}
