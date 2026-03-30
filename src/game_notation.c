#include "chess_app/game_state.h"
#include "game_state_internal.h"

#include <stdio.h>
#include <string.h>

static char piece_letter(ChessPiece piece)
{
    switch (piece) {
    case CHESS_PIECE_WHITE_KNIGHT:
    case CHESS_PIECE_BLACK_KNIGHT:
        return 'N';
    case CHESS_PIECE_WHITE_BISHOP:
    case CHESS_PIECE_BLACK_BISHOP:
        return 'B';
    case CHESS_PIECE_WHITE_ROOK:
    case CHESS_PIECE_BLACK_ROOK:
        return 'R';
    case CHESS_PIECE_WHITE_QUEEN:
    case CHESS_PIECE_BLACK_QUEEN:
        return 'Q';
    case CHESS_PIECE_WHITE_KING:
    case CHESS_PIECE_BLACK_KING:
        return 'K';
    default:
        return '\0';
    }
}

static bool needs_san_disambiguation(
    const ChessGameState *state,
    ChessPiece moving_piece,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank,
    uint8_t promotion,
    bool *out_need_file,
    bool *out_need_rank)
{
    ChessPlayerColor color;
    int rank;
    int file;
    bool file_conflict = false;
    bool rank_conflict = false;

    if (!state || !out_need_file || !out_need_rank) {
        return false;
    }

    *out_need_file = false;
    *out_need_rank = false;

    color = chess_gs_piece_color(moving_piece);
    if (color == CHESS_COLOR_UNASSIGNED ||
        moving_piece == CHESS_PIECE_WHITE_PAWN || moving_piece == CHESS_PIECE_BLACK_PAWN ||
        moving_piece == CHESS_PIECE_WHITE_KING || moving_piece == CHESS_PIECE_BLACK_KING) {
        return false;
    }

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            if (file == from_file && rank == from_rank) {
                continue;
            }
            if ((ChessPiece)state->board[rank][file] != moving_piece) {
                continue;
            }
            if (chess_gs_is_legal_move(state, color, file, rank, to_file, to_rank, promotion)) {
                if (file == from_file) {
                    file_conflict = true;
                }
                if (rank == from_rank) {
                    rank_conflict = true;
                }
                if (file != from_file && rank != from_rank) {
                    *out_need_file = true;
                    return true;
                }
            }
        }
    }

    if (file_conflict) {
        *out_need_rank = true;
    }
    if (rank_conflict || (!file_conflict && !rank_conflict && (*out_need_rank))) {
        *out_need_file = true;
    }
    if (file_conflict && rank_conflict) {
        *out_need_file = true;
        *out_need_rank = true;
    }

    return *out_need_file || *out_need_rank;
}

static char promotion_letter(uint8_t promotion)
{
    switch (promotion) {
    case CHESS_PROMOTION_QUEEN:
        return 'Q';
    case CHESS_PROMOTION_ROOK:
        return 'R';
    case CHESS_PROMOTION_BISHOP:
        return 'B';
    case CHESS_PROMOTION_KNIGHT:
        return 'N';
    default:
        return '\0';
    }
}

bool chess_move_format_algebraic_notation(
    const ChessGameState *state,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank,
    uint8_t promotion,
    char *out,
    size_t out_size)
{
    ChessPiece moving_piece;
    ChessPiece target_piece;
    ChessPlayerColor moving_color;
    bool is_capture;
    bool is_castling;
    bool is_en_passant;
    char san[32];
    size_t len = 0;
    ChessGameState after;
    ChessPlayerColor next_color;

    if (!out || out_size == 0) {
        return false;
    }

    out[0] = '\0';

    if (!state || !chess_gs_in_bounds(from_file, from_rank) || !chess_gs_in_bounds(to_file, to_rank)) {
        return false;
    }

    moving_piece = (ChessPiece)state->board[from_rank][from_file];
    target_piece = (ChessPiece)state->board[to_rank][to_file];
    moving_color = chess_gs_piece_color(moving_piece);

    if (moving_piece == CHESS_PIECE_EMPTY || moving_color == CHESS_COLOR_UNASSIGNED) {
        return false;
    }

    if (!chess_gs_is_legal_move(state, moving_color, from_file, from_rank, to_file, to_rank, promotion)) {
        return false;
    }

    is_castling = chess_gs_is_castling_king_move(moving_piece, from_file, from_rank, to_file, to_rank);
    is_en_passant = chess_gs_is_en_passant_capture_move(state, moving_piece, from_file, from_rank, to_file, to_rank);
    is_capture = (target_piece != CHESS_PIECE_EMPTY) || is_en_passant;

    memset(san, 0, sizeof(san));

    if (is_castling) {
        (void)snprintf(san, sizeof(san), "%s", (to_file == 6) ? "O-O" : "O-O-O");
        len = strlen(san);
    } else if (moving_piece == CHESS_PIECE_WHITE_PAWN || moving_piece == CHESS_PIECE_BLACK_PAWN) {
        if (is_capture) {
            san[len++] = (char)('a' + from_file);
            san[len++] = 'x';
        }
        san[len++] = (char)('a' + to_file);
        san[len++] = (char)('8' - to_rank);
        if (chess_gs_is_pawn_promotion_move(moving_piece, to_rank)) {
            char promo = promotion_letter(promotion);
            if (promo != '\0') {
                san[len++] = '=';
                san[len++] = promo;
            }
        }
        san[len] = '\0';
    } else {
        bool need_file = false;
        bool need_rank = false;
        char p = piece_letter(moving_piece);

        if (p == '\0') {
            return false;
        }

        san[len++] = p;
        (void)needs_san_disambiguation(
            state,
            moving_piece,
            from_file,
            from_rank,
            to_file,
            to_rank,
            promotion,
            &need_file,
            &need_rank);
        if (need_file) {
            san[len++] = (char)('a' + from_file);
        }
        if (need_rank) {
            san[len++] = (char)('8' - from_rank);
        }
        if (is_capture) {
            san[len++] = 'x';
        }
        san[len++] = (char)('a' + to_file);
        san[len++] = (char)('8' - to_rank);
        san[len] = '\0';
    }

    after = *state;
    if (!chess_gs_apply_move(&after, from_file, from_rank, to_file, to_rank, promotion)) {
        return false;
    }

    next_color = after.side_to_move;
    if (chess_gs_is_king_in_check(&after, next_color)) {
        if (!chess_gs_has_any_legal_move(&after, next_color)) {
            san[len++] = '#';
        } else {
            san[len++] = '+';
        }
        san[len] = '\0';
    }

    (void)snprintf(out, out_size, "%s", san);
    return true;
}
