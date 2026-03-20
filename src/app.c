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
    unsigned int hello_failures;
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
    hello_failures = 0u;
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
            if (!connect_attempted || now >= next_connect_attempt_at) {
                connect_attempted = true;
                next_connect_attempt_at = now + (uint64_t)connect_retry_ms;

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

                    memset(&local_hello, 0, sizeof(local_hello));
                    memset(&remote_hello, 0, sizeof(remote_hello));
                    SDL_strlcpy(local_hello.uuid, network_session.local_peer.uuid, sizeof(local_hello.uuid));
                    local_hello.role = (uint32_t)network_session.role;

                    if (chess_tcp_send_hello(&connection, &local_hello) &&
                        chess_tcp_recv_hello(&connection, 500, &remote_hello)) {
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
