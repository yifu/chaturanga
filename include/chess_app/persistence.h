#ifndef CHESS_APP_PERSISTENCE_H
#define CHESS_APP_PERSISTENCE_H

#include "chess_app/app_context.h"
#include "chess_app/network_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool chess_persist_load_snapshot_metadata(
    uint32_t game_id,
    char *out_white_profile_id,
    size_t out_white_size,
    char *out_black_profile_id,
    size_t out_black_size,
    char *out_resume_token,
    size_t out_resume_token_size);

bool chess_persist_load_match_snapshot(AppContext *ctx, uint32_t game_id, const char *expected_resume_token);
bool chess_persist_save_match_snapshot(AppContext *ctx);

bool chess_persist_build_state_snapshot_payload(const AppContext *ctx, ChessStateSnapshotPayload *out_payload);
bool chess_persist_apply_state_snapshot_payload(AppContext *ctx, const ChessStateSnapshotPayload *payload, bool validate_resume_token);

bool chess_persist_load_client_resume_state(AppContext *ctx);
bool chess_persist_save_client_resume_state(AppContext *ctx);
void chess_persist_clear_client_resume_state(AppContext *ctx);

#endif
