/**
 * Internal header shared by the split game_state_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/game_state.c
 *   src/game_notation.c
 */
#ifndef CHESS_APP_GAME_STATE_INTERNAL_H
#define CHESS_APP_GAME_STATE_INTERNAL_H

#include "chess_app/game_state.h"

#include <stdbool.h>
#include <stdint.h>

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
