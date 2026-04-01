#include "chess_app/notification.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

void chess_notification_init(void)
{
}

/* Sanitise a string so it is safe inside an osascript double-quoted
 * context.  Only ASCII printable characters are kept; backslashes
 * and double-quotes are escaped. */
static void sanitise(char *dst, size_t dst_size, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_size; ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (ch < 0x20 || ch > 0x7e) {
            continue; /* drop non-printable / non-ASCII */
        }
        if (ch == '"' || ch == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = (char)ch;
    }
    dst[j] = '\0';
}

void chess_notification_send(const char *title, const char *body)
{
    char safe_title[128];
    char safe_body[256];
    char cmd[512];

    if (!title || !body) {
        return;
    }

    sanitise(safe_title, sizeof(safe_title), title);
    sanitise(safe_body, sizeof(safe_body), body);

    SDL_snprintf(cmd, sizeof(cmd),
        "osascript -e 'display notification \"%s\" with title \"%s\" "
        "sound name \"default\"' &",
        safe_body, safe_title);
    (void)system(cmd);
}

void chess_notification_cleanup(void)
{
}
