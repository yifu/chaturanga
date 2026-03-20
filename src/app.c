#include "chess_app/app.h"

#include "chess_app/network_discovery.h"
#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/render_board.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <string.h>

static bool init_local_peer(ChessPeerInfo *local_peer)
{
    if (!local_peer) {
        return false;
    }

    memset(local_peer, 0, sizeof(*local_peer));

    if (!chess_get_default_local_ipv4(&local_peer->ipv4_host_order)) {
        SDL_Log("Could not detect local IPv4 address");
        return false;
    }

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

    chess_network_session_init(&network_session, &local_peer);
    if (!chess_discovery_start(&discovery, &local_peer, listener.port)) {
        SDL_Log("Discovery start failed");
        chess_tcp_listener_close(&listener);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

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
            ChessPeerInfo remote_peer;
            if (chess_discovery_poll(&discovery, &remote_peer)) {
                chess_network_session_set_remote(&network_session, &remote_peer);
                SDL_Log("Peer discovered; starting election");
            }
        }

        chess_network_session_step(&network_session);

        if (network_session.state != last_state) {
            SDL_Log("Network state changed: %d -> %d", (int)last_state, (int)network_session.state);
            last_state = network_session.state;
        }

        if (network_session.state == CHESS_NET_CONNECTING) {
            if (network_session.role == CHESS_ROLE_SERVER) {
                SDL_Log("Local role: SERVER (smaller IP)");
            } else if (network_session.role == CHESS_ROLE_CLIENT) {
                SDL_Log("Local role: CLIENT");
            }
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
    chess_tcp_listener_close(&listener);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
