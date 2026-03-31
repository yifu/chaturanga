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

bool chess_persist_load_match_snapshot(AppContext *ctx, uint32_t game_id, const char *expected_resume_token)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];
    FILE *fp;
    char json[32768];
    size_t bytes_read;
    uint32_t snapshot_game_id;
    uint32_t side_to_move;
    uint32_t fullmove_number;
    uint32_t halfmove_clock;
    uint32_t outcome;
    uint32_t white_castle_k;
    uint32_t white_castle_q;
    uint32_t black_castle_k;
    uint32_t black_castle_q;
    int32_t en_passant_file;
    int32_t en_passant_rank;
    char snapshot_resume_token[CHESS_UUID_STRING_LEN];
    const char *board_key;
    const char *history_key;
    const char *p;
    int idx;

    if (!ctx || game_id == 0u) {
        return false;
    }

    if (!chess_persist_build_snapshot_dir(dir, sizeof(dir))) {
        return false;
    }

    (void)snprintf(path, sizeof(path), "%s/%u.json", dir, game_id);
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

    if (!chess_persist_extract_snapshot_u32(json, "\"game_id\"", &snapshot_game_id) || snapshot_game_id != game_id) {
        return false;
    }

    snapshot_resume_token[0] = '\0';
    if (!chess_persist_extract_snapshot_string(json, "\"resume_token\"", snapshot_resume_token, sizeof(snapshot_resume_token))) {
        return false;
    }
    if (expected_resume_token && expected_resume_token[0] != '\0' &&
        SDL_strncmp(snapshot_resume_token, expected_resume_token, CHESS_UUID_STRING_LEN) != 0) {
        return false;
    }

    if (!chess_persist_extract_snapshot_u32(json, "\"side_to_move\"", &side_to_move) ||
        !chess_persist_extract_snapshot_u32(json, "\"fullmove_number\"", &fullmove_number) ||
        !chess_persist_extract_snapshot_u32(json, "\"halfmove_clock\"", &halfmove_clock) ||
        !chess_persist_extract_snapshot_u32(json, "\"outcome\"", &outcome) ||
        !chess_persist_extract_snapshot_u32(json, "\"white_can_castle_kingside\"", &white_castle_k) ||
        !chess_persist_extract_snapshot_u32(json, "\"white_can_castle_queenside\"", &white_castle_q) ||
        !chess_persist_extract_snapshot_u32(json, "\"black_can_castle_kingside\"", &black_castle_k) ||
        !chess_persist_extract_snapshot_u32(json, "\"black_can_castle_queenside\"", &black_castle_q) ||
        !chess_persist_extract_snapshot_i32(json, "\"en_passant_target_file\"", &en_passant_file) ||
        !chess_persist_extract_snapshot_i32(json, "\"en_passant_target_rank\"", &en_passant_rank)) {
        return false;
    }

    chess_game_state_init(&ctx->game.game_state);

    /* Optional last-move fields (absent in older snapshots) */
    {
        uint32_t has_lm = 0u;
        int32_t lm_ff = -1, lm_fr = -1, lm_tf = -1, lm_tr = -1;
        if (chess_persist_extract_snapshot_u32(json, "\"has_last_move\"", &has_lm) && has_lm != 0u &&
            chess_persist_extract_snapshot_i32(json, "\"last_move_from_file\"", &lm_ff) &&
            chess_persist_extract_snapshot_i32(json, "\"last_move_from_rank\"", &lm_fr) &&
            chess_persist_extract_snapshot_i32(json, "\"last_move_to_file\"", &lm_tf) &&
            chess_persist_extract_snapshot_i32(json, "\"last_move_to_rank\"", &lm_tr)) {
            ctx->game.game_state.has_last_move = true;
            ctx->game.game_state.last_move_from_file = (int8_t)lm_ff;
            ctx->game.game_state.last_move_from_rank = (int8_t)lm_fr;
            ctx->game.game_state.last_move_to_file = (int8_t)lm_tf;
            ctx->game.game_state.last_move_to_rank = (int8_t)lm_tr;
        }
    }

    board_key = strstr(json, "\"board\"");
    if (!board_key) {
        return false;
    }
    p = strchr(board_key, '[');
    if (!p) {
        return false;
    }
    ++p;

    for (idx = 0; idx < CHESS_BOARD_SIZE * CHESS_BOARD_SIZE; ++idx) {
        unsigned int piece = 0u;
        int consumed = 0;

        p = chess_persist_skip_json_ws(p);
        if (!p || *p == '\0') {
            return false;
        }
        if (sscanf(p, "%u%n", &piece, &consumed) != 1 || consumed <= 0) {
            return false;
        }
        if (piece >= CHESS_PIECE_COUNT) {
            return false;
        }
        ctx->game.game_state.board[idx / CHESS_BOARD_SIZE][idx % CHESS_BOARD_SIZE] = (uint8_t)piece;
        p += consumed;
        p = chess_persist_skip_json_ws(p);
        if (*p == ',') {
            ++p;
        }
    }

    ctx->game.game_state.side_to_move = (ChessPlayerColor)side_to_move;
    ctx->game.game_state.fullmove_number = (uint16_t)fullmove_number;
    ctx->game.game_state.halfmove_clock = (uint16_t)halfmove_clock;
    ctx->game.game_state.outcome = (ChessGameOutcome)outcome;
    ctx->game.game_state.white_can_castle_kingside = (white_castle_k != 0u);
    ctx->game.game_state.white_can_castle_queenside = (white_castle_q != 0u);
    ctx->game.game_state.black_can_castle_kingside = (black_castle_k != 0u);
    ctx->game.game_state.black_can_castle_queenside = (black_castle_q != 0u);
    ctx->game.game_state.en_passant_target_file = (int8_t)en_passant_file;
    ctx->game.game_state.en_passant_target_rank = (int8_t)en_passant_rank;
    ctx->game.game_state.has_selection = false;
    ctx->game.game_state.selected_file = -1;
    ctx->game.game_state.selected_rank = -1;

    /* Optional clock fields (absent in older snapshots) */
    {
        uint32_t tc = 0u, wr = 0u, br = 0u;
        if (chess_persist_extract_snapshot_u32(json, "\"time_control_ms\"", &tc)) {
            ctx->game.time_control_ms = tc;
        } else {
            ctx->game.time_control_ms = CHESS_DEFAULT_TIME_CONTROL_MS;
        }
        if (chess_persist_extract_snapshot_u32(json, "\"white_remaining_ms\"", &wr) &&
            chess_persist_extract_snapshot_u32(json, "\"black_remaining_ms\"", &br)) {
            ctx->game.white_remaining_ms = wr;
            ctx->game.black_remaining_ms = br;
        } else {
            ctx->game.white_remaining_ms = ctx->game.time_control_ms;
            ctx->game.black_remaining_ms = ctx->game.time_control_ms;
        }
        ctx->game.last_clock_sync_ticks = SDL_GetTicks();
    }

    memset(ctx->game.game_state.captured, 0, sizeof(ctx->game.game_state.captured));
    {
        const char *cap_key = strstr(json, "\"captured\"");
        if (cap_key) {
            const char *cap_p = strchr(cap_key, '[');
            if (cap_p) {
                int cap_idx;
                ++cap_p;
                for (cap_idx = 0; cap_idx < CHESS_PIECE_COUNT; ++cap_idx) {
                    unsigned int cap_val = 0u;
                    int cap_consumed = 0;
                    cap_p = chess_persist_skip_json_ws(cap_p);
                    if (!cap_p || *cap_p == '\0' || *cap_p == ']') {
                        break;
                    }
                    if (sscanf(cap_p, "%u%n", &cap_val, &cap_consumed) != 1 || cap_consumed <= 0) {
                        break;
                    }
                    ctx->game.game_state.captured[cap_idx] = (uint8_t)cap_val;
                    cap_p += cap_consumed;
                    cap_p = chess_persist_skip_json_ws(cap_p);
                    if (*cap_p == ',') {
                        ++cap_p;
                    }
                }
            }
        }
    }

    ctx->game.move_history_count = 0;
    history_key = strstr(json, "\"move_history\"");
    if (!history_key) {
        return false;
    }
    p = strchr(history_key, '[');
    if (!p) {
        return false;
    }
    ++p;

    while (ctx->game.move_history_count < (uint16_t)APP_MOVE_HISTORY_MAX) {
        const char *start;
        const char *end;
        size_t len;

        p = chess_persist_skip_json_ws(p);
        if (!p || *p == '\0' || *p == ']') {
            break;
        }
        start = strchr(p, '"');
        if (!start) {
            break;
        }
        ++start;
        end = strchr(start, '"');
        if (!end) {
            break;
        }

        len = (size_t)(end - start);
        if (len >= (size_t)APP_MOVE_HISTORY_ENTRY) {
            len = (size_t)APP_MOVE_HISTORY_ENTRY - 1u;
        }
        memcpy(ctx->game.move_history[ctx->game.move_history_count], start, len);
        ctx->game.move_history[ctx->game.move_history_count][len] = '\0';
        ctx->game.move_history_count += 1u;

        p = end + 1;
        p = chess_persist_skip_json_ws(p);
        if (*p == ',') {
            ++p;
        }
    }

    return true;
}

bool chess_persist_save_match_snapshot(AppContext *ctx)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char tmp_path[PATH_MAX];
    const char *white_profile_id;
    const char *black_profile_id;
    FILE *fp;
    int rank;
    int file;

    if (!ctx || !ctx->network.network_session.game_started || ctx->network.network_session.game_id == 0u) {
        return false;
    }

    if (!chess_persist_build_snapshot_dir(dir, sizeof(dir))) {
        return false;
    }

    (void)snprintf(path, sizeof(path), "%s/%u.json", dir, ctx->network.network_session.game_id);
    (void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    white_profile_id = (ctx->network.network_session.local_color == CHESS_COLOR_WHITE)
        ? ctx->network.local_peer.profile_id
        : ctx->network.network_session.remote_peer.profile_id;
    black_profile_id = (ctx->network.network_session.local_color == CHESS_COLOR_BLACK)
        ? ctx->network.local_peer.profile_id
        : ctx->network.network_session.remote_peer.profile_id;

    fp = fopen(tmp_path, "wb");
    if (!fp) {
        return false;
    }

    (void)fprintf(fp, "{\n");
    (void)fprintf(fp, "  \"version\": 1,\n");
    (void)fprintf(fp, "  \"game_id\": %u,\n", ctx->network.network_session.game_id);
    (void)fprintf(fp, "  \"white_profile_id\": \"%s\",\n", white_profile_id ? white_profile_id : "");
    (void)fprintf(fp, "  \"black_profile_id\": \"%s\",\n", black_profile_id ? black_profile_id : "");
    (void)fprintf(fp, "  \"resume_token\": \"%s\",\n", ctx->protocol.pending_start_payload.resume_token);
    (void)fprintf(fp, "  \"side_to_move\": %u,\n", (unsigned)ctx->game.game_state.side_to_move);
    (void)fprintf(fp, "  \"fullmove_number\": %u,\n", (unsigned)ctx->game.game_state.fullmove_number);
    (void)fprintf(fp, "  \"halfmove_clock\": %u,\n", (unsigned)ctx->game.game_state.halfmove_clock);
    (void)fprintf(fp, "  \"white_can_castle_kingside\": %u,\n", ctx->game.game_state.white_can_castle_kingside ? 1u : 0u);
    (void)fprintf(fp, "  \"white_can_castle_queenside\": %u,\n", ctx->game.game_state.white_can_castle_queenside ? 1u : 0u);
    (void)fprintf(fp, "  \"black_can_castle_kingside\": %u,\n", ctx->game.game_state.black_can_castle_kingside ? 1u : 0u);
    (void)fprintf(fp, "  \"black_can_castle_queenside\": %u,\n", ctx->game.game_state.black_can_castle_queenside ? 1u : 0u);
    (void)fprintf(fp, "  \"en_passant_target_file\": %d,\n", (int)ctx->game.game_state.en_passant_target_file);
    (void)fprintf(fp, "  \"en_passant_target_rank\": %d,\n", (int)ctx->game.game_state.en_passant_target_rank);
    (void)fprintf(fp, "  \"has_last_move\": %u,\n", ctx->game.game_state.has_last_move ? 1u : 0u);
    (void)fprintf(fp, "  \"last_move_from_file\": %d,\n", (int)ctx->game.game_state.last_move_from_file);
    (void)fprintf(fp, "  \"last_move_from_rank\": %d,\n", (int)ctx->game.game_state.last_move_from_rank);
    (void)fprintf(fp, "  \"last_move_to_file\": %d,\n", (int)ctx->game.game_state.last_move_to_file);
    (void)fprintf(fp, "  \"last_move_to_rank\": %d,\n", (int)ctx->game.game_state.last_move_to_rank);
    (void)fprintf(fp, "  \"outcome\": %u,\n", (unsigned)ctx->game.game_state.outcome);
    (void)fprintf(fp, "  \"time_control_ms\": %u,\n", (unsigned)ctx->game.time_control_ms);
    (void)fprintf(fp, "  \"white_remaining_ms\": %u,\n", (unsigned)ctx->game.white_remaining_ms);
    (void)fprintf(fp, "  \"black_remaining_ms\": %u,\n", (unsigned)ctx->game.black_remaining_ms);
    (void)fprintf(fp, "  \"captured\": [");
    for (file = 0; file < CHESS_PIECE_COUNT; ++file) {
        (void)fprintf(fp, "%u", (unsigned)ctx->game.game_state.captured[file]);
        if (file + 1 < CHESS_PIECE_COUNT) {
            (void)fprintf(fp, ",");
        }
    }
    (void)fprintf(fp, "],\n");
    (void)fprintf(fp, "  \"board\": [");
    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            (void)fprintf(fp, "%u", (unsigned)ctx->game.game_state.board[rank][file]);
            if (!(rank == CHESS_BOARD_SIZE - 1 && file == CHESS_BOARD_SIZE - 1)) {
                (void)fprintf(fp, ",");
            }
        }
    }
    (void)fprintf(fp, "],\n");
    (void)fprintf(fp, "  \"move_history\": [");
    for (file = 0; file < (int)ctx->game.move_history_count; ++file) {
        (void)fprintf(fp, "\"%s\"", ctx->game.move_history[file]);
        if (file + 1 < (int)ctx->game.move_history_count) {
            (void)fprintf(fp, ",");
        }
    }
    (void)fprintf(fp, "]\n");
    (void)fprintf(fp, "}\n");

    if (fclose(fp) != 0) {
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        return false;
    }
    return true;
}
