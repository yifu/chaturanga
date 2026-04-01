#ifndef CHESS_APP_NOTIFICATION_H
#define CHESS_APP_NOTIFICATION_H

void chess_notification_init(void);
void chess_notification_send(const char *title, const char *body);
void chess_notification_cleanup(void);

#endif
