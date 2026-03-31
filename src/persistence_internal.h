/**
 * Internal header shared by the split persistence_*.c modules.
 *
 * Not part of the public API — only #included by:
 *   src/persistence.c
 *   src/persistence_resume.c
 *   src/persistence_snapshot.c
 */
#ifndef CHESS_APP_PERSISTENCE_INTERNAL_H
#define CHESS_APP_PERSISTENCE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Shared internal helpers (defined in persistence.c)                 */
/* ------------------------------------------------------------------ */

bool chess_persist_build_resume_state_path(char *out_path, size_t out_path_size);
bool chess_persist_extract_snapshot_string(const char *json, const char *key, char *out_value, size_t out_size);
bool chess_persist_extract_snapshot_u32(const char *json, const char *key, uint32_t *out_value);
bool chess_persist_extract_snapshot_i32(const char *json, const char *key, int32_t *out_value);
bool chess_persist_build_snapshot_dir(char *out_dir, size_t out_dir_size);
const char *chess_persist_skip_json_ws(const char *p);

#endif /* CHESS_APP_PERSISTENCE_INTERNAL_H */
