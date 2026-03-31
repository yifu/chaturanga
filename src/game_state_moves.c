#include "chess_app/game_state.h"
#include "game_state_internal.h"

/* ── Path validation ─────────────────────────────────────────────────── */

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

    dist_file = chess_gs_abs_i(to_file - from_file);
    dist_rank = chess_gs_abs_i(to_rank - from_rank);
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

/* ── Pseudo-legal move check ─────────────────────────────────────────── */

static bool can_castle(
    const ChessGameState *state,
    ChessPlayerColor moving_color,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank);

static bool is_square_attacked(const ChessGameState *state, int target_file, int target_rank, ChessPlayerColor by_color);

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

    if (!state || !chess_gs_in_bounds(from_file, from_rank) || !chess_gs_in_bounds(to_file, to_rank)) {
        return false;
    }

    if (from_file == to_file && from_rank == to_rank) {
        return false;
    }

    target_piece = (ChessPiece)state->board[to_rank][to_file];
    if (chess_gs_piece_color(target_piece) == chess_gs_piece_color(piece)) {
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

        if (chess_gs_abs_i(df) == 1 && dr == dir) {
            if (target_piece != CHESS_PIECE_EMPTY) {
                return true;
            }
            return chess_gs_is_en_passant_capture_move(state, piece, from_file, from_rank, to_file, to_rank);
        }
        return false;
    }
    case CHESS_PIECE_WHITE_KNIGHT:
    case CHESS_PIECE_BLACK_KNIGHT:
        return (chess_gs_abs_i(df) == 1 && chess_gs_abs_i(dr) == 2) || (chess_gs_abs_i(df) == 2 && chess_gs_abs_i(dr) == 1);
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
        if (chess_gs_abs_i(df) <= 1 && chess_gs_abs_i(dr) <= 1) {
            return true;
        }
        return can_castle(state, chess_gs_piece_color(piece), from_file, from_rank, to_file, to_rank);
    case CHESS_PIECE_EMPTY:
    case CHESS_PIECE_COUNT:
    default:
        return false;
    }
}

/* ── King and attack detection ───────────────────────────────────────── */

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

    if (!state || !chess_gs_in_bounds(target_file, target_rank)) {
        return false;
    }

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            ChessPiece piece = (ChessPiece)state->board[rank][file];
            int df = target_file - file;
            int dr = target_rank - rank;

            if (piece == CHESS_PIECE_EMPTY || chess_gs_piece_color(piece) != by_color) {
                continue;
            }

            switch (piece) {
            case CHESS_PIECE_WHITE_PAWN:
                if (dr == -1 && chess_gs_abs_i(df) == 1) {
                    return true;
                }
                break;
            case CHESS_PIECE_BLACK_PAWN:
                if (dr == 1 && chess_gs_abs_i(df) == 1) {
                    return true;
                }
                break;
            case CHESS_PIECE_WHITE_KNIGHT:
            case CHESS_PIECE_BLACK_KNIGHT:
                if ((chess_gs_abs_i(df) == 1 && chess_gs_abs_i(dr) == 2) || (chess_gs_abs_i(df) == 2 && chess_gs_abs_i(dr) == 1)) {
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
                if (chess_gs_abs_i(df) <= 1 && chess_gs_abs_i(dr) <= 1) {
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

/* ── Check and castling ──────────────────────────────────────────────── */

bool chess_gs_is_king_in_check(const ChessGameState *state, ChessPlayerColor color)
{
    int king_file;
    int king_rank;

    if (!state || !find_king(state, color, &king_file, &king_rank)) {
        return false;
    }

    return is_square_attacked(state, king_file, king_rank, chess_gs_opposite_color(color));
}

static bool can_castle(
    const ChessGameState *state,
    ChessPlayerColor moving_color,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank)
{
    ChessPlayerColor enemy_color = chess_gs_opposite_color(moving_color);

    if (!state || enemy_color == CHESS_COLOR_UNASSIGNED) {
        return false;
    }

    if (moving_color == CHESS_COLOR_WHITE) {
        if (from_file != 4 || from_rank != 7 || to_rank != 7) {
            return false;
        }

        if (to_file == 6) {
            if (!state->white_can_castle_kingside ||
                (ChessPiece)state->board[7][7] != CHESS_PIECE_WHITE_ROOK ||
                (ChessPiece)state->board[7][5] != CHESS_PIECE_EMPTY ||
                (ChessPiece)state->board[7][6] != CHESS_PIECE_EMPTY) {
                return false;
            }
            return !is_square_attacked(state, 4, 7, enemy_color) &&
                   !is_square_attacked(state, 5, 7, enemy_color) &&
                   !is_square_attacked(state, 6, 7, enemy_color);
        }

        if (to_file == 2) {
            if (!state->white_can_castle_queenside ||
                (ChessPiece)state->board[7][0] != CHESS_PIECE_WHITE_ROOK ||
                (ChessPiece)state->board[7][1] != CHESS_PIECE_EMPTY ||
                (ChessPiece)state->board[7][2] != CHESS_PIECE_EMPTY ||
                (ChessPiece)state->board[7][3] != CHESS_PIECE_EMPTY) {
                return false;
            }
            return !is_square_attacked(state, 4, 7, enemy_color) &&
                   !is_square_attacked(state, 3, 7, enemy_color) &&
                   !is_square_attacked(state, 2, 7, enemy_color);
        }

        return false;
    }

    if (moving_color == CHESS_COLOR_BLACK) {
        if (from_file != 4 || from_rank != 0 || to_rank != 0) {
            return false;
        }

        if (to_file == 6) {
            if (!state->black_can_castle_kingside ||
                (ChessPiece)state->board[0][7] != CHESS_PIECE_BLACK_ROOK ||
                (ChessPiece)state->board[0][5] != CHESS_PIECE_EMPTY ||
                (ChessPiece)state->board[0][6] != CHESS_PIECE_EMPTY) {
                return false;
            }
            return !is_square_attacked(state, 4, 0, enemy_color) &&
                   !is_square_attacked(state, 5, 0, enemy_color) &&
                   !is_square_attacked(state, 6, 0, enemy_color);
        }

        if (to_file == 2) {
            if (!state->black_can_castle_queenside ||
                (ChessPiece)state->board[0][0] != CHESS_PIECE_BLACK_ROOK ||
                (ChessPiece)state->board[0][1] != CHESS_PIECE_EMPTY ||
                (ChessPiece)state->board[0][2] != CHESS_PIECE_EMPTY ||
                (ChessPiece)state->board[0][3] != CHESS_PIECE_EMPTY) {
                return false;
            }
            return !is_square_attacked(state, 4, 0, enemy_color) &&
                   !is_square_attacked(state, 3, 0, enemy_color) &&
                   !is_square_attacked(state, 2, 0, enemy_color);
        }

        return false;
    }

    return false;
}

/* ── Legal move validation ───────────────────────────────────────────── */

bool chess_gs_is_legal_move(
    const ChessGameState *state,
    ChessPlayerColor moving_color,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank,
    uint8_t promotion)
{
    ChessGameState copy;
    ChessPiece piece;

    if (!state || !chess_gs_in_bounds(from_file, from_rank) || !chess_gs_in_bounds(to_file, to_rank)) {
        return false;
    }

    piece = (ChessPiece)state->board[from_rank][from_file];
    if (piece == CHESS_PIECE_EMPTY || chess_gs_piece_color(piece) != moving_color) {
        return false;
    }

    if (!is_pseudo_legal_move(state, piece, from_file, from_rank, to_file, to_rank)) {
        return false;
    }

    if (chess_gs_is_pawn_promotion_move(piece, to_rank)) {
        if (!chess_gs_is_valid_promotion_choice(promotion)) {
            return false;
        }
    } else if (promotion != CHESS_PROMOTION_NONE) {
        return false;
    }

    copy = *state;
    copy.board[to_rank][to_file] = copy.board[from_rank][from_file];
    copy.board[from_rank][from_file] = CHESS_PIECE_EMPTY;

    if (chess_gs_is_en_passant_capture_move(state, piece, from_file, from_rank, to_file, to_rank)) {
        copy.board[from_rank][to_file] = CHESS_PIECE_EMPTY;
    }

    if (chess_gs_is_castling_king_move(piece, from_file, from_rank, to_file, to_rank)) {
        if (to_file == 6) {
            copy.board[to_rank][5] = copy.board[to_rank][7];
            copy.board[to_rank][7] = CHESS_PIECE_EMPTY;
        } else {
            copy.board[to_rank][3] = copy.board[to_rank][0];
            copy.board[to_rank][0] = CHESS_PIECE_EMPTY;
        }
    }

    if (chess_gs_is_pawn_promotion_move(piece, to_rank)) {
        ChessPiece promoted = chess_gs_promoted_piece_for_choice(moving_color, promotion);
        if (promoted == CHESS_PIECE_EMPTY) {
            return false;
        }
        copy.board[to_rank][to_file] = (uint8_t)promoted;
    }

    return !chess_gs_is_king_in_check(&copy, moving_color);
}

bool chess_gs_has_any_legal_move(const ChessGameState *state, ChessPlayerColor color)
{
    int from_rank;
    int from_file;
    int to_rank;
    int to_file;
    ChessPiece piece;

    for (from_rank = 0; from_rank < CHESS_BOARD_SIZE; ++from_rank) {
        for (from_file = 0; from_file < CHESS_BOARD_SIZE; ++from_file) {
            piece = (ChessPiece)state->board[from_rank][from_file];
            if (piece == CHESS_PIECE_EMPTY || chess_gs_piece_color(piece) != color) {
                continue;
            }
            for (to_rank = 0; to_rank < CHESS_BOARD_SIZE; ++to_rank) {
                for (to_file = 0; to_file < CHESS_BOARD_SIZE; ++to_file) {
                    if (chess_gs_is_pawn_promotion_move(piece, to_rank)) {
                        if (chess_gs_is_legal_move(state, color, from_file, from_rank, to_file, to_rank, CHESS_PROMOTION_QUEEN) ||
                            chess_gs_is_legal_move(state, color, from_file, from_rank, to_file, to_rank, CHESS_PROMOTION_ROOK) ||
                            chess_gs_is_legal_move(state, color, from_file, from_rank, to_file, to_rank, CHESS_PROMOTION_BISHOP) ||
                            chess_gs_is_legal_move(state, color, from_file, from_rank, to_file, to_rank, CHESS_PROMOTION_KNIGHT)) {
                            return true;
                        }
                    } else if (chess_gs_is_legal_move(
                                   state,
                                   color,
                                   from_file,
                                   from_rank,
                                   to_file,
                                   to_rank,
                                   CHESS_PROMOTION_NONE)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}
