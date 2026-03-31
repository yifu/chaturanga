#include "chess_app/persistence.h"
#include "persistence_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/game_state.h"
#include "chess_app/network_protocol.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Network snapshot payload ─────────────────────────────────────────── */

bool chess_persist_build_state_snapshot_payload(const AppContext *ctx, ChessStateSnapshotPayload *out_payload)
{
    uint16_t history_count;
    int rank;
    int file;
    int idx;

    if (!ctx || !out_payload || !ctx->network.network_session.game_started || ctx->network.network_session.game_id == 0u) {
        return false;
    }

    memset(out_payload, 0, sizeof(*out_payload));
    out_payload->game_id = ctx->network.network_session.game_id;
    out_payload->side_to_move = (uint32_t)ctx->game.game_state.side_to_move;
    out_payload->fullmove_number = (uint32_t)ctx->game.game_state.fullmove_number;
    out_payload->halfmove_clock = (uint32_t)ctx->game.game_state.halfmove_clock;
    out_payload->outcome = (uint32_t)ctx->game.game_state.outcome;
    out_payload->white_can_castle_kingside = ctx->game.game_state.white_can_castle_kingside ? 1u : 0u;
    out_payload->white_can_castle_queenside = ctx->game.game_state.white_can_castle_queenside ? 1u : 0u;
    out_payload->black_can_castle_kingside = ctx->game.game_state.black_can_castle_kingside ? 1u : 0u;
    out_payload->black_can_castle_queenside = ctx->game.game_state.black_can_castle_queenside ? 1u : 0u;
    out_payload->en_passant_target_file = ctx->game.game_state.en_passant_target_file;
    out_payload->en_passant_target_rank = ctx->game.game_state.en_passant_target_rank;
    out_payload->has_last_move = ctx->game.game_state.has_last_move ? 1u : 0u;
    out_payload->last_move_from_file = ctx->game.game_state.last_move_from_file;
    out_payload->last_move_from_rank = ctx->game.game_state.last_move_from_rank;
    out_payload->last_move_to_file = ctx->game.game_state.last_move_to_file;
    out_payload->last_move_to_rank = ctx->game.game_state.last_move_to_rank;
    SDL_strlcpy(out_payload->resume_token, ctx->protocol.pending_start_payload.resume_token, sizeof(out_payload->resume_token));

    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            idx = rank * CHESS_BOARD_SIZE + file;
            out_payload->board[idx] = ctx->game.game_state.board[rank][file];
        }
    }

    memcpy(out_payload->captured, ctx->game.game_state.captured,
           sizeof(out_payload->captured) < sizeof(ctx->game.game_state.captured)
               ? sizeof(out_payload->captured)
               : sizeof(ctx->game.game_state.captured));

    history_count = ctx->game.move_history_count;
    if (history_count > (uint16_t)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES) {
        history_count = (uint16_t)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES;
    }
    out_payload->move_history_count = history_count;

    for (idx = 0; idx < (int)history_count; ++idx) {
        SDL_strlcpy(
            out_payload->move_history[idx],
            ctx->game.move_history[idx],
            sizeof(out_payload->move_history[idx]));
    }

    return true;
}

bool chess_persist_apply_state_snapshot_payload(
    AppContext *ctx,
    const ChessStateSnapshotPayload *payload,
    bool validate_resume_token)
{
    uint16_t history_count;
    int idx;

    if (!ctx || !payload || payload->game_id == 0u) {
        return false;
    }

    if (payload->side_to_move != CHESS_COLOR_WHITE && payload->side_to_move != CHESS_COLOR_BLACK) {
        return false;
    }
    if (payload->outcome > CHESS_OUTCOME_FIFTY_MOVE_RULE) {
        return false;
    }

    if (validate_resume_token &&
        ctx->protocol.pending_start_payload.resume_token[0] != '\0' &&
        SDL_strncmp(
            payload->resume_token,
            ctx->protocol.pending_start_payload.resume_token,
            CHESS_UUID_STRING_LEN) != 0) {
        return false;
    }

    chess_game_state_init(&ctx->game.game_state);
    for (idx = 0; idx < (int)CHESS_PROTOCOL_SNAPSHOT_BOARD_CELLS; ++idx) {
        if (payload->board[idx] >= CHESS_PIECE_COUNT) {
            return false;
        }
        ctx->game.game_state.board[idx / CHESS_BOARD_SIZE][idx % CHESS_BOARD_SIZE] = payload->board[idx];
    }

    ctx->game.game_state.side_to_move = (ChessPlayerColor)payload->side_to_move;
    ctx->game.game_state.fullmove_number = (uint16_t)payload->fullmove_number;
    ctx->game.game_state.halfmove_clock = (uint16_t)payload->halfmove_clock;
    ctx->game.game_state.outcome = (ChessGameOutcome)payload->outcome;
    ctx->game.game_state.white_can_castle_kingside = payload->white_can_castle_kingside != 0u;
    ctx->game.game_state.white_can_castle_queenside = payload->white_can_castle_queenside != 0u;
    ctx->game.game_state.black_can_castle_kingside = payload->black_can_castle_kingside != 0u;
    ctx->game.game_state.black_can_castle_queenside = payload->black_can_castle_queenside != 0u;
    ctx->game.game_state.en_passant_target_file = payload->en_passant_target_file;
    ctx->game.game_state.en_passant_target_rank = payload->en_passant_target_rank;
    ctx->game.game_state.has_last_move = payload->has_last_move != 0u;
    ctx->game.game_state.last_move_from_file = payload->last_move_from_file;
    ctx->game.game_state.last_move_from_rank = payload->last_move_from_rank;
    ctx->game.game_state.last_move_to_file = payload->last_move_to_file;
    ctx->game.game_state.last_move_to_rank = payload->last_move_to_rank;
    ctx->game.game_state.has_selection = false;
    ctx->game.game_state.selected_file = -1;
    ctx->game.game_state.selected_rank = -1;

    memcpy(ctx->game.game_state.captured, payload->captured,
           sizeof(ctx->game.game_state.captured) < sizeof(payload->captured)
               ? sizeof(ctx->game.game_state.captured)
               : sizeof(payload->captured));

    history_count = payload->move_history_count;
    if (history_count > (uint16_t)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES) {
        history_count = (uint16_t)CHESS_PROTOCOL_MAX_MOVE_HISTORY_ENTRIES;
    }

    ctx->game.move_history_count = history_count;
    for (idx = 0; idx < (int)history_count; ++idx) {
        SDL_strlcpy(
            ctx->game.move_history[idx],
            payload->move_history[idx],
            (size_t)APP_MOVE_HISTORY_ENTRY);
    }

    return true;
}

/* ── Client resume state ──────────────────────────────────────────────── */

bool chess_persist_save_client_resume_state(AppContext *ctx)
{
    char path[PATH_MAX];
    char tmp_path[PATH_MAX];
    FILE *fp;

    if (!ctx ||
        ctx->protocol.pending_start_payload.game_id == 0u ||
        ctx->protocol.pending_start_payload.resume_token[0] == '\0' ||
        ctx->network.network_session.remote_peer.profile_id[0] == '\0') {
        return false;
    }

    if (!chess_persist_build_resume_state_path(path, sizeof(path))) {
        return false;
    }

    (void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    fp = fopen(tmp_path, "wb");
    if (!fp) {
        return false;
    }

    (void)fprintf(fp, "{\n");
    (void)fprintf(fp, "  \"version\": 1,\n");
    (void)fprintf(fp, "  \"game_id\": %u,\n", ctx->protocol.pending_start_payload.game_id);
    (void)fprintf(fp, "  \"resume_token\": \"%s\",\n", ctx->protocol.pending_start_payload.resume_token);
    (void)fprintf(fp, "  \"remote_profile_id\": \"%s\"\n", ctx->network.network_session.remote_peer.profile_id);
    (void)fprintf(fp, "}\n");

    if (fclose(fp) != 0) {
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        return false;
    }

    ctx->resume.resume_state_loaded = true;
    SDL_strlcpy(
        ctx->resume.resume_remote_profile_id,
        ctx->network.network_session.remote_peer.profile_id,
        sizeof(ctx->resume.resume_remote_profile_id));
    return true;
}

void chess_persist_clear_client_resume_state(AppContext *ctx)
{
    char path[PATH_MAX];

    if (!ctx) {
        return;
    }

    if (chess_persist_build_resume_state_path(path, sizeof(path))) {
        (void)remove(path);
    }

    ctx->resume.resume_state_loaded = false;
    ctx->resume.resume_remote_profile_id[0] = '\0';
    ctx->protocol.pending_start_payload.game_id = 0u;
    ctx->protocol.pending_start_payload.resume_token[0] = '\0';
}

bool chess_persist_load_client_resume_state(AppContext *ctx)
{
    char path[PATH_MAX];
    FILE *fp;
    char json[1024];
    size_t bytes_read;
    uint32_t game_id;
    char resume_token[CHESS_UUID_STRING_LEN];
    char remote_profile_id[CHESS_PROFILE_ID_STRING_LEN];

    if (!ctx) {
        return false;
    }

    if (!chess_persist_build_resume_state_path(path, sizeof(path))) {
        return false;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    bytes_read = fread(json, 1, sizeof(json) - 1u, fp);
    (void)fclose(fp);
    if (bytes_read == 0u) {
        return false;
    }
    json[bytes_read] = '\0';

    if (!chess_persist_extract_snapshot_u32(json, "\"game_id\"", &game_id)) {
        return false;
    }
    if (!chess_persist_extract_snapshot_string(json, "\"resume_token\"", resume_token, sizeof(resume_token))) {
        return false;
    }
    if (!chess_persist_extract_snapshot_string(json, "\"remote_profile_id\"", remote_profile_id, sizeof(remote_profile_id))) {
        return false;
    }

    ctx->protocol.pending_start_payload.game_id = game_id;
    SDL_strlcpy(
        ctx->protocol.pending_start_payload.resume_token,
        resume_token,
        sizeof(ctx->protocol.pending_start_payload.resume_token));
    SDL_strlcpy(
        ctx->resume.resume_remote_profile_id,
        remote_profile_id,
        sizeof(ctx->resume.resume_remote_profile_id));
    ctx->resume.resume_state_loaded = true;
    return true;
}
