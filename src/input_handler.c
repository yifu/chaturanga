#include "chess_app/input_handler.h"
#include "input_internal.h"

#include "chess_app/app_context.h"
#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_session.h"
#include "chess_app/persistence.h"
#include "chess_app/transport.h"
#include "chess_app/ui_game.h"
#include "chess_app/ui_lobby.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---------- clipboard helper ---------- */

/* History-panel layout constants (mirrored from game_panels.c) */
#define BTN_HEIGHT     28
#define BTN_MARGIN      8

static void copy_move_history_to_clipboard(AppContext *ctx)
{
    size_t capacity;
    char *buffer;
    int turn;
    int total_turns;

    if (!ctx) {
        return;
    }

    if (ctx->game.move_history_count == 0u) {
        app_set_status_message(ctx, "No moves to copy", 1200u);
        return;
    }

    total_turns = ((int)ctx->game.move_history_count + 1) / 2;
    capacity = ((size_t)total_turns * 48u) + 1u;
    buffer = (char *)SDL_malloc(capacity);
    if (!buffer) {
        app_set_status_message(ctx, "Failed to allocate clipboard buffer", 1800u);
        return;
    }

    buffer[0] = '\0';
    for (turn = 1; turn <= total_turns; ++turn) {
        char line[96];
        int white_idx = (turn - 1) * 2;
        int black_idx = white_idx + 1;
        const char *white_move = (white_idx < (int)ctx->game.move_history_count) ? ctx->game.move_history[white_idx] : "";
        const char *black_move = (black_idx < (int)ctx->game.move_history_count) ? ctx->game.move_history[black_idx] : "";

        if (black_move[0] != '\0') {
            SDL_snprintf(line, sizeof(line), "%d. %s %s\n", turn, white_move, black_move);
        } else {
            SDL_snprintf(line, sizeof(line), "%d. %s\n", turn, white_move);
        }
        SDL_strlcat(buffer, line, capacity);
    }

    if (SDL_SetClipboardText(buffer)) {
        app_set_status_message(ctx, "Move history copied to clipboard", 1400u);
    } else {
        app_set_status_message(ctx, "Failed to copy move history", 1800u);
    }
    SDL_free(buffer);
}

/* ---------- lobby interaction ---------- */

static void handle_lobby_click(AppContext *ctx, int clicked_peer)
{
    if (!ctx || clicked_peer < 0 || clicked_peer >= ctx->game.lobby.discovered_peer_count) {
        return;
    }

    {
        ChessChallengeState current_state = chess_lobby_get_challenge_state(&ctx->game.lobby, clicked_peer);

        if (current_state == CHESS_CHALLENGE_NONE) {
            /* Send a challenge to this peer — does not affect other challenges */
            chess_lobby_set_challenge_state(&ctx->game.lobby, clicked_peer, CHESS_CHALLENGE_OUTGOING_PENDING);
            SDL_Log("LOBBY: challenge sent to peer %d (%.8s...)", clicked_peer, ctx->game.lobby.discovered_peers[clicked_peer].peer.profile_id);
        } else if (current_state == CHESS_CHALLENGE_OUTGOING_PENDING) {
            /* Cancel challenge for this specific peer */
            chess_lobby_set_challenge_state(&ctx->game.lobby, clicked_peer, CHESS_CHALLENGE_NONE);
            chess_lobby_close_challenge_connection(&ctx->game.lobby, clicked_peer);
            SDL_Log("LOBBY: challenge cancelled for peer %d", clicked_peer);
        } else if (current_state == CHESS_CHALLENGE_CONNECT_FAILED) {
            /* Retry: reset connection state and re-attempt */
            chess_lobby_close_challenge_connection(&ctx->game.lobby, clicked_peer);
            chess_lobby_set_challenge_state(&ctx->game.lobby, clicked_peer, CHESS_CHALLENGE_OUTGOING_PENDING);
            SDL_Log("LOBBY: retrying challenge to peer %d (%.8s...)", clicked_peer, ctx->game.lobby.discovered_peers[clicked_peer].peer.profile_id);
        } else if (current_state == CHESS_CHALLENGE_INCOMING_PENDING) {
            /* Accept an incoming challenge — promote to game session */
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_profile_id, ctx->network.network_session.local_peer.profile_id, sizeof(accept.acceptor_profile_id));

            if (transport_send_accept(&ctx->network.transport.base, &accept)) {
                /* Close all outgoing challenge connections — we're starting a game */
                chess_lobby_close_all_challenge_connections(&ctx->game.lobby);

                ctx->network.network_session.challenge_done = true;
                ctx->network.network_session.role = CHESS_ROLE_SERVER;
                chess_lobby_set_challenge_state(&ctx->game.lobby, clicked_peer, CHESS_CHALLENGE_MATCHED);
                chess_network_session_set_remote(&ctx->network.network_session, &ctx->game.lobby.discovered_peers[clicked_peer].peer);
                chess_network_session_set_phase(&ctx->network.network_session, CHESS_PHASE_GAME_STARTING);
                SDL_Log("LOBBY: accepted challenge from peer %d (%.8s...)", clicked_peer, ctx->game.lobby.discovered_peers[clicked_peer].peer.profile_id);
            } else {
                SDL_Log("NET: cannot accept challenge yet, transport not ready");
            }
        }
    }
}

/* ---------- promotion helper ---------- */

static uint8_t key_to_promotion_choice(SDL_Keycode key)
{
    switch (key) {
    case SDLK_Q:
        return CHESS_PROMOTION_QUEEN;
    case SDLK_R:
        return CHESS_PROMOTION_ROOK;
    case SDLK_B:
        return CHESS_PROMOTION_BISHOP;
    case SDLK_N:
        return CHESS_PROMOTION_KNIGHT;
    default:
        return CHESS_PROMOTION_NONE;
    }
}

/* ---------- resign / draw button handling ---------- */

static void handle_game_button(AppContext *ctx, ChessGameButton btn)
{
    if (!ctx) {
        return;
    }

    /* Return to lobby works even without a connection */
    if (btn == CHESS_GAME_BUTTON_RETURN_LOBBY) {
        app_return_to_lobby(ctx);
        return;
    }

    if (transport_get_fd(&ctx->network.transport.base) < 0) {
        return;
    }

    switch (btn) {
    case CHESS_GAME_BUTTON_RESIGN: {
        ctx->game.game_state.outcome = (ctx->network.network_session.local_color == CHESS_COLOR_WHITE)
            ? CHESS_OUTCOME_WHITE_RESIGNED
            : CHESS_OUTCOME_BLACK_RESIGNED;
        if (!transport_send_packet(&ctx->network.transport.base, CHESS_MSG_RESIGN,
                              ctx->protocol.move_sequence, NULL, 0u)) {
            app_handle_peer_disconnect(ctx, "failed to send RESIGN");
            return;
        }
        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log("GAME: local player resigned");
        app_set_status_message(ctx, "You resigned.", 5000u);
        break;
    }
    case CHESS_GAME_BUTTON_DRAW:
        ctx->network.network_session.draw_offer_pending = true;
        if (!transport_send_packet(&ctx->network.transport.base, CHESS_MSG_DRAW_OFFER,
                              ctx->protocol.move_sequence, NULL, 0u)) {
            app_handle_peer_disconnect(ctx, "failed to send DRAW_OFFER");
            return;
        }
        SDL_Log("GAME: draw offer sent");
        app_set_status_message(ctx, "Draw offer sent.", 3000u);
        break;
    case CHESS_GAME_BUTTON_ACCEPT_DRAW:
        ctx->game.game_state.outcome = CHESS_OUTCOME_DRAW_AGREED;
        ctx->network.network_session.draw_offer_received = false;
        if (!transport_send_packet(&ctx->network.transport.base, CHESS_MSG_DRAW_ACCEPT,
                              ctx->protocol.move_sequence, NULL, 0u)) {
            app_handle_peer_disconnect(ctx, "failed to send DRAW_ACCEPT");
            return;
        }
        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log("GAME: draw accepted locally");
        app_set_status_message(ctx, "Draw by agreement.", 5000u);
        break;
    case CHESS_GAME_BUTTON_DECLINE_DRAW:
        ctx->network.network_session.draw_offer_received = false;
        if (!transport_send_packet(&ctx->network.transport.base, CHESS_MSG_DRAW_DECLINE,
                              ctx->protocol.move_sequence, NULL, 0u)) {
            app_handle_peer_disconnect(ctx, "failed to send DRAW_DECLINE");
            return;
        }
        SDL_Log("GAME: draw declined locally");
        app_set_status_message(ctx, "Draw declined.", 3000u);
        break;
    default:
        break;
    }
}

/* ---------- public entry point ---------- */

void chess_input_handle_events(AppContext *ctx)
{
    SDL_Event event;

    if (!ctx) {
        return;
    }

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            ctx->running = false;
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            ctx->win.window_has_focus = true;
            continue;
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            ctx->win.window_has_focus = false;
            continue;
        }

        if (!ctx->network.network_session.game_started) {
            if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                int ww = 0;
                int wh = 0;
                SDL_GetWindowSize(ctx->win.window, &ww, &wh);
                {
                    int row_step = 36 + 6; /* peer_row_height + peer_row_gap */
                    int total_content = ctx->game.lobby.discovered_peer_count * row_step;
                    int visible_area = wh - 60; /* margin + title area */
                    int max_scroll = total_content - visible_area;
                    int delta = (event.wheel.y > 0.0f) ? -row_step : ((event.wheel.y < 0.0f) ? row_step : 0);

                    if (max_scroll < 0) {
                        max_scroll = 0;
                    }
                    ctx->game.lobby.scroll_offset += delta;
                    if (ctx->game.lobby.scroll_offset < 0) {
                        ctx->game.lobby.scroll_offset = 0;
                    }
                    if (ctx->game.lobby.scroll_offset > max_scroll) {
                        ctx->game.lobby.scroll_offset = max_scroll;
                    }
                }
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                ctx->game.lobby.discovered_peer_count > 0) {
                const int clicked_peer = chess_lobby_find_clicked_peer(ctx->win.window, &ctx->game.lobby, event.button.x, event.button.y);
                if (clicked_peer >= 0) {
                    handle_lobby_click(ctx, clicked_peer);
                }
            }
            continue;
        }

        if (transport_get_fd(&ctx->network.transport.base) < 0) {
            /* Still allow clicking the "Return to Lobby" overlay button */
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                ChessGameButton btn = chess_ui_game_button_from_mouse(ctx, event.button.x, event.button.y);
                if (btn == CHESS_GAME_BUTTON_RETURN_LOBBY) {
                    handle_game_button(ctx, btn);
                }
            }
            continue;
        }

        if (event.type == SDL_EVENT_KEY_DOWN) {
            SDL_Keymod mods = SDL_GetModState();
            if ((mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0 && event.key.key == SDLK_C) {
                copy_move_history_to_clipboard(ctx);
                continue;
            }
        }

        if (event.type == SDL_EVENT_KEY_DOWN && ctx->ui.drag.promotion_pending) {
            if (event.key.key == SDLK_ESCAPE) {
                ctx->ui.drag.promotion_pending = false;
                ctx->ui.drag.promotion_to_file = -1;
                ctx->ui.drag.promotion_to_rank = -1;
                chess_game_clear_selection(&ctx->game.game_state);
                app_set_status_message(ctx, "Promotion cancelled", 1200u);
                continue;
            }

            {
                uint8_t promotion = key_to_promotion_choice(event.key.key);
                if (promotion != CHESS_PROMOTION_NONE) {
                    (void)chess_input_try_send_local_move(
                        ctx,
                        ctx->ui.drag.promotion_to_file,
                        ctx->ui.drag.promotion_to_rank,
                        promotion);
                    continue;
                }
            }
        }

        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            int ww = 0;
            int wh = 0;
            SDL_GetWindowSize(ctx->win.window, &ww, &wh);
            {
                int bw = chess_ui_board_width_for_window(ww, true);
                if ((int)event.wheel.mouse_x >= bw && ctx->game.move_history_count > 0) {
                    int total_turns = ((int)ctx->game.move_history_count + 1) / 2;
                    int btn_top = wh - BTN_HEIGHT - BTN_MARGIN - BTN_MARGIN;
                    int max_rows = (btn_top - 64) / 20;
                    int max_offset = total_turns - max_rows;
                    int scroll_lines = (event.wheel.y > 0.0f) ? 1 : ((event.wheel.y < 0.0f) ? -1 : 0);

                    if (max_offset < 0) {
                        max_offset = 0;
                    }
                    ctx->ui.history_scroll_offset += scroll_lines;
                    if (ctx->ui.history_scroll_offset < 0) {
                        ctx->ui.history_scroll_offset = 0;
                    }
                    if (ctx->ui.history_scroll_offset > max_offset) {
                        ctx->ui.history_scroll_offset = max_offset;
                    }
                }
            }
            continue;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            ChessGameButton btn = chess_ui_game_button_from_mouse(ctx, event.button.x, event.button.y);
            if (btn != CHESS_GAME_BUTTON_NONE) {
                handle_game_button(ctx, btn);
                continue;
            }
            chess_input_handle_board_mouse_down(ctx, event.button.x, event.button.y);
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            chess_input_handle_board_mouse_motion(ctx, event.motion.x, event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            chess_input_handle_board_mouse_up(ctx, event.button.x, event.button.y);
        }
    }
}
