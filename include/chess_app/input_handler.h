#ifndef CHESS_APP_INPUT_HANDLER_H
#define CHESS_APP_INPUT_HANDLER_H

typedef struct AppContext AppContext;
typedef AppContext AppLoopContext;

/* Process all pending SDL events (keyboard, mouse, quit).
 * Called once per frame from the main loop. */
void chess_input_handle_events(AppLoopContext *ctx);

#endif /* CHESS_APP_INPUT_HANDLER_H */
