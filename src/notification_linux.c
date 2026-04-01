#include "chess_app/notification.h"

#include <libnotify/notify.h>

void chess_notification_init(void)
{
    notify_init("Chaturanga");
}

void chess_notification_send(const char *title, const char *body)
{
    NotifyNotification *n;

    if (!title || !body) {
        return;
    }

    n = notify_notification_new(title, body, NULL);
    notify_notification_show(n, NULL);
    g_object_unref(G_OBJECT(n));
}

void chess_notification_cleanup(void)
{
    notify_uninit();
}
