#include "chess_app/persistence.h"
#include "persistence_internal.h"

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

bool chess_persist_ensure_directory_recursive(const char *path)
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
        return chess_persist_ensure_directory_recursive(out_dir);
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

    return chess_persist_ensure_directory_recursive(out_dir);
}

bool chess_persist_build_snapshot_dir(char *out_dir, size_t out_dir_size)
{
    char base_dir[PATH_MAX];

    if (!out_dir || out_dir_size == 0u) {
        return false;
    }

    if (!build_app_state_dir(base_dir, sizeof(base_dir))) {
        return false;
    }

    (void)snprintf(out_dir, out_dir_size, "%s/matches", base_dir);
    return chess_persist_ensure_directory_recursive(out_dir);
}

bool chess_persist_build_resume_state_path(char *out_path, size_t out_path_size)
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

bool chess_persist_extract_snapshot_string(const char *json, const char *key, char *out_value, size_t out_size)
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

bool chess_persist_extract_snapshot_u32(const char *json, const char *key, uint32_t *out_value)
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

bool chess_persist_extract_snapshot_i32(const char *json, const char *key, int32_t *out_value)
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

const char *chess_persist_skip_json_ws(const char *p)
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

    if (!chess_persist_extract_snapshot_string(json, "\"white_profile_id\"", out_white_profile_id, out_white_size)) {
        return false;
    }
    if (!chess_persist_extract_snapshot_string(json, "\"black_profile_id\"", out_black_profile_id, out_black_size)) {
        return false;
    }
    if (!chess_persist_extract_snapshot_string(json, "\"resume_token\"", out_resume_token, out_resume_token_size)) {
        return false;
    }

    return true;
}
