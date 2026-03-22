#include "chess_app/app.h"

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/render_board.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint32_t make_game_id(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer)
{
    uint32_t hash = 2166136261u;
    const char *uuids[2] = { NULL, NULL };
    int i = 0;
    int j = 0;

    if (!local_peer || !remote_peer) {
        return 0u;
    }

    uuids[0] = (SDL_strncmp(local_peer->uuid, remote_peer->uuid, CHESS_UUID_STRING_LEN) <= 0)
        ? local_peer->uuid
        : remote_peer->uuid;
    uuids[1] = (uuids[0] == local_peer->uuid) ? remote_peer->uuid : local_peer->uuid;

    for (i = 0; i < 2; ++i) {
        for (j = 0; uuids[i][j] != '\0'; ++j) {
            hash ^= (uint8_t)uuids[i][j];
            hash *= 16777619u;
        }
    }

    return hash;
}

static bool init_local_peer(ChessPeerInfo *local_peer)
{
    if (!local_peer) {
        return false;
    }

    memset(local_peer, 0, sizeof(*local_peer));

    if (!chess_generate_peer_uuid(local_peer->uuid, sizeof(local_peer->uuid))) {
        SDL_Log("Could not generate local peer UUID");
        return false;
    }

    SDL_Log("Local peer initialized (uuid=%s)", local_peer->uuid);
    return true;
}

int app_run(void)
{
    const int window_size = 640;
    const int connect_retry_ms = 1000;
    const int hello_timeout_ms = 1200;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDL3 Chess Board", window_size, window_size, 0);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    ChessPeerInfo local_peer;
    ChessNetworkSession network_session;
    ChessDiscoveryContext discovery;
    ChessTcpListener listener;
    ChessTcpConnection connection;
    ChessDiscoveredPeer discovered_peer;
    bool connect_attempted;
    bool hello_completed;
    bool start_completed;
    unsigned int hello_failures;
    unsigned int start_failures;
    uint64_t next_connect_attempt_at;
    ChessNetworkState last_state;

    if (!init_local_peer(&local_peer)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!chess_tcp_listener_open(&listener, 0)) {
        SDL_Log("Could not create TCP listener on ephemeral port");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Log("TCP listener ready on port %u", (unsigned int)listener.port);

    connection.fd = -1;
    memset(&discovered_peer, 0, sizeof(discovered_peer));
    connect_attempted = false;
    hello_completed = false;
    start_completed = false;
    hello_failures = 0u;
    start_failures = 0u;
    next_connect_attempt_at = 0;

    if (!chess_discovery_start(&discovery, &local_peer, listener.port)) {
        SDL_Log("Discovery start failed");
        chess_tcp_listener_close(&listener);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    chess_network_session_init(&network_session, &local_peer);

    last_state = network_session.state;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        if (!network_session.peer_available) {
            if (chess_discovery_poll(&discovery, &discovered_peer)) {
                chess_network_session_set_remote(&network_session, &discovered_peer.peer);
                SDL_Log(
                    "Peer discovered; starting election (remote port=%u)",
                    (unsigned int)discovered_peer.tcp_port
                );
            }
        }

        if (network_session.state == CHESS_NET_CONNECTING && !hello_completed) {
            const uint64_t now = SDL_GetTicks();
            bool should_attempt = false;

            if (network_session.role == CHESS_ROLE_SERVER) {
                /* Server must accept continuously to drain queued stale connects.
                 * If we only accept once per second while clients timeout at ~500 ms,
                 * every accepted connection is already dead and HELLO always fails. */
                should_attempt = true;
            } else if (network_session.role == CHESS_ROLE_CLIENT) {
                if (!connect_attempted || now >= next_connect_attempt_at) {
                    connect_attempted = true;
                    next_connect_attempt_at = now + (uint64_t)connect_retry_ms;
                    should_attempt = true;
                }
            }

            if (should_attempt) {

                if (network_session.role == CHESS_ROLE_SERVER) {
                    if (connection.fd < 0 && chess_tcp_accept_once(&listener, 10, &connection)) {
                        SDL_Log("Accepted TCP client connection");
                    }
                } else if (network_session.role == CHESS_ROLE_CLIENT) {
                    if (connection.fd < 0 &&
                        chess_tcp_connect_once(
                            network_session.remote_peer.ipv4_host_order,
                            discovered_peer.tcp_port,
                            200,
                            &connection
                        )) {
                        SDL_Log("Connected to remote TCP host");
                    }
                }

                if (connection.fd >= 0) {
                    ChessHelloPayload local_hello;
                    ChessHelloPayload remote_hello;
                    ChessAckPayload handshake_ack;
                    bool handshake_ok = false;

                    memset(&local_hello, 0, sizeof(local_hello));
                    memset(&remote_hello, 0, sizeof(remote_hello));
                    memset(&handshake_ack, 0, sizeof(handshake_ack));
                    SDL_strlcpy(local_hello.uuid, network_session.local_peer.uuid, sizeof(local_hello.uuid));
                    local_hello.role = (uint32_t)network_session.role;

                    if (network_session.role == CHESS_ROLE_CLIENT) {
                        /* CLIENT: send -> recv -> send_ack
                         * Ordering ensures SERVER's recv_ack catches stale connections:
                         * if CLIENT already closed, SERVER's recv_ack gets EOF -> fail. */
                        handshake_ok =
                            chess_tcp_send_hello(&connection, &local_hello) &&
                            chess_tcp_recv_hello(&connection, hello_timeout_ms, &remote_hello) &&
                            chess_tcp_send_ack(&connection, CHESS_MSG_HELLO, 1u, 0u);
                    } else {
                        /* SERVER: recv -> send -> recv_ack */
                        handshake_ok =
                            chess_tcp_recv_hello(&connection, hello_timeout_ms, &remote_hello) &&
                            chess_tcp_send_hello(&connection, &local_hello) &&
                            chess_tcp_recv_ack(&connection, hello_timeout_ms, &handshake_ack) &&
                            handshake_ack.acked_message_type == CHESS_MSG_HELLO &&
                            handshake_ack.acked_sequence == 1u &&
                            handshake_ack.status_code == 0u;
                    }

                    if (handshake_ok) {
                        hello_completed = true;
                        chess_network_session_set_transport_ready(&network_session, true);
                        SDL_Log("HELLO handshake completed with peer uuid=%s", remote_hello.uuid);
                    } else {
                        hello_failures += 1u;
                        if (hello_failures == 1u || (hello_failures % 5u) == 0u) {
                            SDL_Log(
                                "HELLO handshake failed (%u failures), will retry connection",
                                hello_failures
                            );
                        }
                        chess_tcp_connection_close(&connection);
                    }
                }
            }
        }

        if (network_session.state == CHESS_NET_IN_GAME && hello_completed && !start_completed) {
            ChessStartPayload start_payload;
            ChessAckPayload start_ack;
            bool start_ok = false;

            memset(&start_payload, 0, sizeof(start_payload));
            memset(&start_ack, 0, sizeof(start_ack));

            if (network_session.role == CHESS_ROLE_SERVER) {
                start_payload.game_id = make_game_id(&network_session.local_peer, &network_session.remote_peer);
                start_payload.assigned_color = CHESS_COLOR_BLACK;
                start_payload.initial_turn = CHESS_COLOR_WHITE;
                SDL_strlcpy(start_payload.white_uuid, network_session.local_peer.uuid, sizeof(start_payload.white_uuid));
                SDL_strlcpy(start_payload.black_uuid, network_session.remote_peer.uuid, sizeof(start_payload.black_uuid));

                start_ok =
                    chess_tcp_send_start(&connection, &start_payload) &&
                    chess_tcp_recv_ack(&connection, 500, &start_ack) &&
                    start_ack.acked_message_type == CHESS_MSG_START &&
                    start_ack.acked_sequence == 2u &&
                    start_ack.status_code == 0u;

                if (start_ok) {
                    chess_network_session_start_game(&network_session, start_payload.game_id, CHESS_COLOR_WHITE);
                }
            } else if (network_session.role == CHESS_ROLE_CLIENT) {
                start_ok =
                    chess_tcp_recv_start(&connection, 500, &start_payload) &&
                    chess_tcp_send_ack(&connection, CHESS_MSG_START, 2u, 0u);

                if (start_ok) {
                    chess_network_session_start_game(
                        &network_session,
                        start_payload.game_id,
                        (ChessPlayerColor)start_payload.assigned_color
                    );
                }
            }

            if (start_ok) {
                start_completed = true;
                SDL_Log(
                    "Game started (game_id=%u, local_color=%s, first_turn=%s)",
                    network_session.game_id,
                    network_session.local_color == CHESS_COLOR_WHITE ? "WHITE" : "BLACK",
                    start_payload.initial_turn == CHESS_COLOR_WHITE ? "WHITE" : "BLACK"
                );
            } else {
                start_failures += 1u;
                if (start_failures == 1u || (start_failures % 5u) == 0u) {
                    SDL_Log("START exchange failed (%u failures), will retry", start_failures);
                }
            }
        }

        chess_network_session_step(&network_session);

        if (network_session.state != last_state) {
            SDL_Log("Network state changed: %d -> %d", (int)last_state, (int)network_session.state);

            if (network_session.state == CHESS_NET_CONNECTING) {
                if (network_session.role == CHESS_ROLE_SERVER) {
                    SDL_Log("Local role: SERVER (smaller IP)");
                } else if (network_session.role == CHESS_ROLE_CLIENT) {
                    SDL_Log("Local role: CLIENT");
                }
            }

            last_state = network_session.state;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_board(renderer, width, height);

        SDL_RenderPresent(renderer);
    }

    chess_discovery_stop(&discovery);
    chess_tcp_connection_close(&connection);
    chess_tcp_listener_close(&listener);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
