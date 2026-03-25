#include "chess_app/input_handler.h"

#include "chess_app/app_context.h"
#include "chess_app/net_handler.h"
#include "chess_app/persistence.h"
#include "chess_app/ui_game.h"
#include "chess_app/ui_lobby.h"

#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"

#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---------- clipboard helper ---------- */

static void copy_move_history_to_clipboard(AppLoopContext *ctx)
{
    size_t capacity;
    char *buffer;
    int turn;
    int total_turns;

    if (!ctx) {
        return;
    }

    if (ctx->move_history_count == 0u) {
        app_set_status_message(ctx, "No moves to copy", 1200u);
        return;
    }

    total_turns = ((int)ctx->move_history_count + 1) / 2;
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
        const char *white_move = (white_idx < (int)ctx->move_history_count) ? ctx->move_history[white_idx] : "";
        const char *black_move = (black_idx < (int)ctx->move_history_count) ? ctx->move_history[black_idx] : "";

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

static void handle_lobby_click(AppLoopContext *ctx, int clicked_peer)
{
    if (!ctx || clicked_peer < 0 || clicked_peer >= ctx->lobby.discovered_peer_count) {
        return;
    }

    ctx->lobby.selected_peer_idx = clicked_peer;

    {
        ChessChallengeState current_state = chess_lobby_get_challenge_state(&ctx->lobby, clicked_peer);

        if (current_state == CHESS_CHALLENGE_NONE) {
            ctx->network_session.role = CHESS_ROLE_CLIENT;
            chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_OUTGOING_PENDING);
            chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[clicked_peer].peer);
            SDL_Log("LOBBY: challenge sent to peer %d (%.8s...)", clicked_peer, ctx->lobby.discovered_peers[clicked_peer].peer.profile_id);
        } else if (current_state == CHESS_CHALLENGE_OUTGOING_PENDING) {
            chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_NONE);
            chess_tcp_connection_close(&ctx->connection);
            chess_net_reset_transport_progress(ctx);
            ctx->network_session.role = CHESS_ROLE_UNKNOWN;
            ctx->network_session.peer_available = false;
            memset(&ctx->network_session.remote_peer, 0, sizeof(ctx->network_session.remote_peer));
            chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_IDLE);
            SDL_Log("LOBBY: challenge cancelled for peer %d", clicked_peer);
        } else if (current_state == CHESS_CHALLENGE_INCOMING_PENDING) {
            ChessAcceptPayload accept;
            memset(&accept, 0, sizeof(accept));
            SDL_strlcpy(accept.acceptor_profile_id, ctx->network_session.local_peer.profile_id, sizeof(accept.acceptor_profile_id));

            if (ctx->connection.fd >= 0 && chess_tcp_send_accept(&ctx->connection, &accept)) {
                ctx->network_session.challenge_done = true;
                ctx->network_session.role = CHESS_ROLE_SERVER;
                chess_lobby_set_challenge_state(&ctx->lobby, clicked_peer, CHESS_CHALLENGE_MATCHED);
                chess_network_session_set_remote(&ctx->network_session, &ctx->lobby.discovered_peers[clicked_peer].peer);
                chess_network_session_set_phase(&ctx->network_session, CHESS_PHASE_GAME_STARTING);
                SDL_Log("LOBBY: accepted challenge from peer %d (%.8s...)", clicked_peer, ctx->lobby.discovered_peers[clicked_peer].peer.profile_id);
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

/* ---------- move sending ---------- */

static bool try_send_local_move(AppLoopContext *ctx, int to_file, int to_rank, uint8_t promotion)
{
    ChessMovePayload move;
    char notation[24];
    bool notation_ready = false;

    if (!ctx || ctx->connection.fd < 0) {
        return false;
    }

    if (ctx->game_state.outcome != CHESS_OUTCOME_NONE) {
        return false;
    }

    if (promotion == CHESS_PROMOTION_NONE &&
        chess_game_local_move_requires_promotion(
            &ctx->game_state,
            ctx->network_session.local_color,
            to_file,
            to_rank)) {
        ctx->promotion_pending = true;
        ctx->promotion_to_file = to_file;
        ctx->promotion_to_rank = to_rank;
        app_set_status_message(ctx, "Promotion: press Q (queen), R (rook), B (bishop), N (knight)", 30000u);
        return false;
    }

    if (ctx->game_state.has_selection &&
        chess_move_format_algebraic_notation(
            &ctx->game_state,
            ctx->game_state.selected_file,
            ctx->game_state.selected_rank,
            to_file,
            to_rank,
            promotion,
            notation,
            sizeof(notation))) {
        notation_ready = true;
    }

    if (!chess_game_try_local_move(
            &ctx->game_state,
            ctx->network_session.local_color,
            to_file,
            to_rank,
            promotion,
            &move)) {
        return false;
    }

    if (notation_ready) {
        app_append_move_history(ctx, notation);
    }

    if (ctx->network_session.role == CHESS_ROLE_SERVER) {
        (void)chess_persist_save_match_snapshot(ctx);
    }

    ctx->promotion_pending = false;
    ctx->promotion_to_file = -1;
    ctx->promotion_to_rank = -1;
    ctx->status_message[0] = '\0';
    ctx->status_message_until_ms = 0;

    /* Playing a move implicitly declines any pending draw offer from opponent. */
    if (ctx->network_session.draw_offer_received) {
        ctx->network_session.draw_offer_received = false;
        chess_tcp_send_packet(&ctx->connection, CHESS_MSG_DRAW_DECLINE,
                              ctx->move_sequence, NULL, 0u);
    }

    if (!chess_tcp_send_packet(
            &ctx->connection,
            CHESS_MSG_MOVE,
            ctx->move_sequence++,
            &move,
            (uint32_t)sizeof(move))) {
        SDL_Log("NET: failed to send MOVE packet, closing connection");
        app_handle_peer_disconnect(ctx, "failed to send MOVE packet");
        return false;
    }

    SDL_Log(
        "GAME: sent local move (%u,%u) -> (%u,%u)",
        (unsigned)move.from_file,
        (unsigned)move.from_rank,
        (unsigned)move.to_file,
        (unsigned)move.to_rank
    );

    return true;
}

/* ---------- resign / draw button handling ---------- */

static void handle_game_button(AppLoopContext *ctx, ChessGameButton btn)
{
    if (!ctx) {
        return;
    }

    /* Return to lobby works even without a connection */
    if (btn == CHESS_GAME_BUTTON_RETURN_LOBBY) {
        app_return_to_lobby(ctx);
        return;
    }

    if (ctx->connection.fd < 0) {
        return;
    }

    switch (btn) {
    case CHESS_GAME_BUTTON_RESIGN: {
        ctx->game_state.outcome = (ctx->network_session.local_color == CHESS_COLOR_WHITE)
            ? CHESS_OUTCOME_WHITE_RESIGNED
            : CHESS_OUTCOME_BLACK_RESIGNED;
        chess_tcp_send_packet(&ctx->connection, CHESS_MSG_RESIGN,
                              ctx->move_sequence, NULL, 0u);
        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log("GAME: local player resigned");
        app_set_status_message(ctx, "You resigned.", 5000u);
        break;
    }
    case CHESS_GAME_BUTTON_DRAW:
        ctx->network_session.draw_offer_pending = true;
        chess_tcp_send_packet(&ctx->connection, CHESS_MSG_DRAW_OFFER,
                              ctx->move_sequence, NULL, 0u);
        SDL_Log("GAME: draw offer sent");
        app_set_status_message(ctx, "Draw offer sent.", 3000u);
        break;
    case CHESS_GAME_BUTTON_ACCEPT_DRAW:
        ctx->game_state.outcome = CHESS_OUTCOME_DRAW_AGREED;
        ctx->network_session.draw_offer_received = false;
        chess_tcp_send_packet(&ctx->connection, CHESS_MSG_DRAW_ACCEPT,
                              ctx->move_sequence, NULL, 0u);
        (void)chess_persist_save_match_snapshot(ctx);
        SDL_Log("GAME: draw accepted locally");
        app_set_status_message(ctx, "Draw by agreement.", 5000u);
        break;
    case CHESS_GAME_BUTTON_DECLINE_DRAW:
        ctx->network_session.draw_offer_received = false;
        chess_tcp_send_packet(&ctx->connection, CHESS_MSG_DRAW_DECLINE,
                              ctx->move_sequence, NULL, 0u);
        SDL_Log("GAME: draw declined locally");
        app_set_status_message(ctx, "Draw declined.", 3000u);
        break;
    default:
        break;
    }
}

/* ---------- board mouse handling ---------- */

static void handle_board_mouse_down(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    int file;
    int rank;

    if (!ctx || ctx->connection.fd < 0) {
        return;
    }

    if (ctx->game_state.outcome != CHESS_OUTCOME_NONE) {
        return;
    }

    if (ctx->promotion_pending) {
        uint8_t promotion = chess_ui_promotion_from_mouse(ctx, mouse_x, mouse_y);
        if (promotion != CHESS_PROMOTION_NONE) {
            (void)try_send_local_move(ctx, ctx->promotion_to_file, ctx->promotion_to_rank, promotion);
        }
        return;
    }

    if (!chess_ui_screen_to_board_square(ctx, mouse_x, mouse_y, &file, &rank)) {
        return;
    }

    if (chess_game_select_local_piece(&ctx->game_state, ctx->network_session.local_color, file, rank)) {
        ctx->drag_active = true;
        ctx->drag_piece = chess_game_get_piece(&ctx->game_state, file, rank);
        ctx->drag_from_file = file;
        ctx->drag_from_rank = rank;
        ctx->drag_mouse_x = mouse_x;
        ctx->drag_mouse_y = mouse_y;
        return;
    }

    if (ctx->game_state.has_selection) {
        if (ctx->game_state.selected_file == file && ctx->game_state.selected_rank == rank) {
            chess_game_clear_selection(&ctx->game_state);
            return;
        }

        (void)try_send_local_move(ctx, file, rank, CHESS_PROMOTION_NONE);
    }
}

static void handle_board_mouse_motion(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    if (!ctx || !ctx->drag_active) {
        return;
    }

    ctx->drag_mouse_x = mouse_x;
    ctx->drag_mouse_y = mouse_y;
}

static void handle_board_mouse_up(AppLoopContext *ctx, int mouse_x, int mouse_y)
{
    int to_file;
    int to_rank;

    if (!ctx || !ctx->drag_active) {
        return;
    }

    ctx->drag_mouse_x = mouse_x;
    ctx->drag_mouse_y = mouse_y;

    if (ctx->promotion_pending) {
        ctx->drag_active = false;
        ctx->drag_piece = CHESS_PIECE_EMPTY;
        ctx->drag_from_file = -1;
        ctx->drag_from_rank = -1;
        return;
    }

    if (chess_ui_screen_to_board_square(ctx, mouse_x, mouse_y, &to_file, &to_rank)) {
        (void)try_send_local_move(ctx, to_file, to_rank, CHESS_PROMOTION_NONE);
    }

    ctx->drag_active = false;
    ctx->drag_piece = CHESS_PIECE_EMPTY;
    ctx->drag_from_file = -1;
    ctx->drag_from_rank = -1;
}

/* ---------- public entry point ---------- */

void chess_input_handle_events(AppLoopContext *ctx)
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

        if (!ctx->network_session.game_started) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                ctx->lobby.discovered_peer_count > 0) {
                const int clicked_peer = chess_lobby_find_clicked_peer(ctx->window, &ctx->lobby, event.button.x, event.button.y);
                if (clicked_peer >= 0) {
                    handle_lobby_click(ctx, clicked_peer);
                }
            }
            continue;
        }

        if (ctx->connection.fd < 0) {
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

        if (event.type == SDL_EVENT_KEY_DOWN && ctx->promotion_pending) {
            if (event.key.key == SDLK_ESCAPE) {
                ctx->promotion_pending = false;
                ctx->promotion_to_file = -1;
                ctx->promotion_to_rank = -1;
                chess_game_clear_selection(&ctx->game_state);
                app_set_status_message(ctx, "Promotion cancelled", 1200u);
                continue;
            }

            {
                uint8_t promotion = key_to_promotion_choice(event.key.key);
                if (promotion != CHESS_PROMOTION_NONE) {
                    (void)try_send_local_move(
                        ctx,
                        ctx->promotion_to_file,
                        ctx->promotion_to_rank,
                        promotion);
                    continue;
                }
            }
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            ChessGameButton btn = chess_ui_game_button_from_mouse(ctx, event.button.x, event.button.y);
            if (btn != CHESS_GAME_BUTTON_NONE) {
                handle_game_button(ctx, btn);
                continue;
            }
            handle_board_mouse_down(ctx, event.button.x, event.button.y);
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            handle_board_mouse_motion(ctx, event.motion.x, event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            handle_board_mouse_up(ctx, event.button.x, event.button.y);
        }
    }
}
