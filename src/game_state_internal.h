/**
 * Internal header shared by the split game_state_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/game_state.c
 *   src/game_state_moves.c
 *   src/game_notation.c
 */
#ifndef CHESS_APP_GAME_STATE_INTERNAL_H
#define CHESS_APP_GAME_STATE_INTERNAL_H

#include "chess_app/game_state.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Inline utility helpers (shared across split modules)               */
/* ------------------------------------------------------------------ */

static inline int chess_gs_abs_i(int v)
{
    return (v < 0) ? -v : v;
}

static inline ChessPlayerColor chess_gs_opposite_color(ChessPlayerColor color)
{
    if (color == CHESS_COLOR_WHITE) {
        return CHESS_COLOR_BLACK;
    }
    if (color == CHESS_COLOR_BLACK) {
        return CHESS_COLOR_WHITE;
    }
    return CHESS_COLOR_UNASSIGNED;
}

static inline bool chess_gs_is_valid_promotion_choice(uint8_t promotion)
{
    return promotion == CHESS_PROMOTION_QUEEN ||
           promotion == CHESS_PROMOTION_ROOK ||
           promotion == CHESS_PROMOTION_BISHOP ||
           promotion == CHESS_PROMOTION_KNIGHT;
}

static inline ChessPiece chess_gs_promoted_piece_for_choice(ChessPlayerColor color, uint8_t promotion)
{
    if (color == CHESS_COLOR_WHITE) {
        switch (promotion) {
        case CHESS_PROMOTION_QUEEN:  return CHESS_PIECE_WHITE_QUEEN;
        case CHESS_PROMOTION_ROOK:   return CHESS_PIECE_WHITE_ROOK;
        case CHESS_PROMOTION_BISHOP: return CHESS_PIECE_WHITE_BISHOP;
        case CHESS_PROMOTION_KNIGHT: return CHESS_PIECE_WHITE_KNIGHT;
        default:                     return CHESS_PIECE_EMPTY;
        }
    }
    if (color == CHESS_COLOR_BLACK) {
        switch (promotion) {
        case CHESS_PROMOTION_QUEEN:  return CHESS_PIECE_BLACK_QUEEN;
        case CHESS_PROMOTION_ROOK:   return CHESS_PIECE_BLACK_ROOK;
        case CHESS_PROMOTION_BISHOP: return CHESS_PIECE_BLACK_BISHOP;
        case CHESS_PROMOTION_KNIGHT: return CHESS_PIECE_BLACK_KNIGHT;
        default:                     return CHESS_PIECE_EMPTY;
        }
    }
    return CHESS_PIECE_EMPTY;
}

/* ------------------------------------------------------------------ */
/*  Utility helpers                                                    */
/* ------------------------------------------------------------------ */

bool chess_gs_in_bounds(int file, int rank);
ChessPlayerColor chess_gs_piece_color(ChessPiece piece);
bool chess_gs_is_castling_king_move(ChessPiece piece, int from_file, int from_rank, int to_file, int to_rank);
bool chess_gs_is_en_passant_capture_move(
    const ChessGameState *state, ChessPiece piece,
    int from_file, int from_rank, int to_file, int to_rank);
bool chess_gs_is_pawn_promotion_move(ChessPiece piece, int to_rank);

/* ------------------------------------------------------------------ */
/*  Move validation                                                    */
/* ------------------------------------------------------------------ */

bool chess_gs_is_legal_move(
    const ChessGameState *state, ChessPlayerColor color,
    int from_file, int from_rank, int to_file, int to_rank,
    uint8_t promotion);
bool chess_gs_is_king_in_check(const ChessGameState *state, ChessPlayerColor color);
bool chess_gs_has_any_legal_move(const ChessGameState *state, ChessPlayerColor color);

/* ------------------------------------------------------------------ */
/*  Move execution                                                     */
/* ------------------------------------------------------------------ */

bool chess_gs_apply_move(
    ChessGameState *state,
    int from_file, int from_rank, int to_file, int to_rank,
    uint8_t promotion);

#endif /* CHESS_APP_GAME_STATE_INTERNAL_H */
