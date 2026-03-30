#include "chess_app/persistence.h"

#include "chess_app/app_context.h"
#include "chess_app/game_state.h"
#include "chess_app/network_protocol.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Internal helpers ─────────────────────────────────────────────────── */

static bool ensure_directory_recursive(const char *path)
{
    char tmp[PATH_MAX];
    char *p;
    size_t len;

    if (!path || path[0] == '\0') {
        return false;
    }

    (void)snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) {
        return false;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool build_app_state_dir(char *out_dir, size_t out_dir_size)
{
    const char *override_dir = getenv("CHESS_APP_PROFILE_DIR");
    const char *home = getenv("HOME");
    const char *xdg_state_home = getenv("XDG_STATE_HOME");

    if (!out_dir || out_dir_size == 0) {
        return false;
    }

    if (override_dir && override_dir[0] != '\0') {
        (void)snprintf(out_dir, out_dir_size, "%s", override_dir);
        return ensure_directory_recursive(out_dir);
    }

    if (!home || home[0] == '\0') {
        return false;
    }

#if defined(__APPLE__)
    (void)snprintf(out_dir, out_dir_size, "%s/Library/Application Support/chess_app", home);
#else
    if (xdg_state_home && xdg_state_home[0] != '\0') {
        (void)snprintf(out_dir, out_dir_size, "%s/chess_app", xdg_state_home);
    } else {
        (void)snprintf(out_dir, out_dir_size, "%s/.local/state/chess_app", home);
    }
#endif

    return ensure_directory_recursive(out_dir);
}

static bool build_snapshot_dir(char *out_dir, size_t out_dir_size)
{
    char base_dir[PATH_MAX];

    if (!out_dir || out_dir_size == 0u) {
        return false;
    }

    if (!build_app_state_dir(base_dir, sizeof(base_dir))) {
        return false;
    }

    (void)snprintf(out_dir, out_dir_size, "%s/matches", base_dir);
    return ensure_directory_recursive(out_dir);
}

static bool build_resume_state_path(char *out_path, size_t out_path_size)
{
    char base_dir[PATH_MAX];

    if (!out_path || out_path_size == 0u) {
        return false;
    }

    if (!build_app_state_dir(base_dir, sizeof(base_dir))) {
        return false;
    }

    (void)snprintf(out_path, out_path_size, "%s/resume_state.json", base_dir);
    return true;
}

static bool extract_snapshot_string(const char *json, const char *key, char *out_value, size_t out_size)
{
    const char *pos;
    const char *start;
    const char *end;
    size_t len;

    if (!json || !key || !out_value || out_size == 0u) {
        return false;
    }

    pos = strstr(json, key);
    if (!pos) {
        return false;
    }

    start = strchr(pos, '"');
    if (!start) {
        return false;
    }
    start = strchr(start + 1, '"');
    if (!start) {
        return false;
    }
    start = strchr(start + 1, '"');
    if (!start) {
        return false;
    }
    start += 1;
    end = strchr(start, '"');
    if (!end || end < start) {
        return false;
    }

    len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1u;
    }
    memcpy(out_value, start, len);
    out_value[len] = '\0';
    return true;
}

static bool extract_snapshot_u32(const char *json, const char *key, uint32_t *out_value)
{
    const char *pos;
    const char *colon;
    unsigned int parsed;

    if (!json || !key || !out_value) {
        return false;
    }

    pos = strstr(json, key);
    if (!pos) {
        return false;
    }

    colon = strchr(pos, ':');
    if (!colon) {
        return false;
    }

    if (sscanf(colon + 1, " %u", &parsed) != 1) {
        return false;
    }

    *out_value = (uint32_t)parsed;
    return true;
}

static bool extract_snapshot_i32(const char *json, const char *key, int32_t *out_value)
{
    const char *pos;
    const char *colon;
    int parsed;

    if (!json || !key || !out_value) {
        return false;
    }

    pos = strstr(json, key);
    if (!pos) {
        return false;
    }

    colon = strchr(pos, ':');
    if (!colon) {
        return false;
    }

    if (sscanf(colon + 1, " %d", &parsed) != 1) {
        return false;
    }

    *out_value = (int32_t)parsed;
    return true;
}

static const char *skip_json_ws(const char *p)
{
    if (!p) {
        return NULL;
    }

    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        ++p;
    }
    return p;
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool chess_persist_load_snapshot_metadata(
    uint32_t game_id,
    char *out_white_profile_id,
    size_t out_white_size,
    char *out_black_profile_id,
    size_t out_black_size,
    char *out_resume_token,
    size_t out_resume_token_size)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];
    FILE *fp;
    char json[8192];
    size_t bytes_read;
    uint32_t snapshot_game_id;

    if (!out_white_profile_id || !out_black_profile_id || !out_resume_token) {
        return false;
    }

    out_white_profile_id[0] = '\0';
    out_black_profile_id[0] = '\0';
    out_resume_token[0] = '\0';

    if (!build_snapshot_dir(dir, sizeof(dir))) {
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

    if (!extract_snapshot_u32(json, "\"game_id\"", &snapshot_game_id) || snapshot_game_id != game_id) {
        return false;
    }

    if (!extract_snapshot_string(json, "\"white_profile_id\"", out_white_profile_id, out_white_size)) {
        return false;
    }
    if (!extract_snapshot_string(json, "\"black_profile_id\"", out_black_profile_id, out_black_size)) {
        return false;
    }
    if (!extract_snapshot_string(json, "\"resume_token\"", out_resume_token, out_resume_token_size)) {
        return false;
    }

    return true;
}

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

    if (!build_snapshot_dir(dir, sizeof(dir))) {
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

    if (!extract_snapshot_u32(json, "\"game_id\"", &snapshot_game_id) || snapshot_game_id != game_id) {
        return false;
    }

    snapshot_resume_token[0] = '\0';
    if (!extract_snapshot_string(json, "\"resume_token\"", snapshot_resume_token, sizeof(snapshot_resume_token))) {
        return false;
    }
    if (expected_resume_token && expected_resume_token[0] != '\0' &&
        SDL_strncmp(snapshot_resume_token, expected_resume_token, CHESS_UUID_STRING_LEN) != 0) {
        return false;
    }

    if (!extract_snapshot_u32(json, "\"side_to_move\"", &side_to_move) ||
        !extract_snapshot_u32(json, "\"fullmove_number\"", &fullmove_number) ||
        !extract_snapshot_u32(json, "\"halfmove_clock\"", &halfmove_clock) ||
        !extract_snapshot_u32(json, "\"outcome\"", &outcome) ||
        !extract_snapshot_u32(json, "\"white_can_castle_kingside\"", &white_castle_k) ||
        !extract_snapshot_u32(json, "\"white_can_castle_queenside\"", &white_castle_q) ||
        !extract_snapshot_u32(json, "\"black_can_castle_kingside\"", &black_castle_k) ||
        !extract_snapshot_u32(json, "\"black_can_castle_queenside\"", &black_castle_q) ||
        !extract_snapshot_i32(json, "\"en_passant_target_file\"", &en_passant_file) ||
        !extract_snapshot_i32(json, "\"en_passant_target_rank\"", &en_passant_rank)) {
        return false;
    }

    chess_game_state_init(&ctx->game.game_state);

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

        p = skip_json_ws(p);
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
        p = skip_json_ws(p);
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
                    cap_p = skip_json_ws(cap_p);
                    if (!cap_p || *cap_p == '\0' || *cap_p == ']') {
                        break;
                    }
                    if (sscanf(cap_p, "%u%n", &cap_val, &cap_consumed) != 1 || cap_consumed <= 0) {
                        break;
                    }
                    ctx->game.game_state.captured[cap_idx] = (uint8_t)cap_val;
                    cap_p += cap_consumed;
                    cap_p = skip_json_ws(cap_p);
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

        p = skip_json_ws(p);
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
        p = skip_json_ws(p);
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

    if (!build_snapshot_dir(dir, sizeof(dir))) {
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
    (void)fprintf(fp, "  \"outcome\": %u,\n", (unsigned)ctx->game.game_state.outcome);
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

    if (!build_resume_state_path(path, sizeof(path))) {
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

    if (build_resume_state_path(path, sizeof(path))) {
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

    if (!build_resume_state_path(path, sizeof(path))) {
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

    if (!extract_snapshot_u32(json, "\"game_id\"", &game_id)) {
        return false;
    }
    if (!extract_snapshot_string(json, "\"resume_token\"", resume_token, sizeof(resume_token))) {
        return false;
    }
    if (!extract_snapshot_string(json, "\"remote_profile_id\"", remote_profile_id, sizeof(remote_profile_id))) {
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
