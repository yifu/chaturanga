#ifndef CHESS_APP_RESUME_CONTEXT_H
#define CHESS_APP_RESUME_CONTEXT_H

#include "chess_app/network_protocol.h"

#include <stdbool.h>

typedef struct ResumeContext {
    bool resume_state_loaded;
    char resume_remote_profile_id[CHESS_PROFILE_ID_STRING_LEN];
} ResumeContext;

#endif
