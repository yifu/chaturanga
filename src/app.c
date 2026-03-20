#include "chess_app/app.h"

#include "chess_app/network_peer.h"
#include "chess_app/network_session.h"
#include "chess_app/render_board.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <string.h>

static void log_host_election_example(void)
{
    ChessPeerInfo local_peer;
    ChessPeerInfo remote_peer;

    memset(&local_peer, 0, sizeof(local_peer));
    memset(&remote_peer, 0, sizeof(remote_peer));

    if (!chess_parse_ipv4("192.168.1.12", &local_peer.ipv4_host_order)) {
        SDL_Log("Failed to parse local peer IP");
        return;
    }

    if (!chess_parse_ipv4("192.168.1.48", &remote_peer.ipv4_host_order)) {
        SDL_Log("Failed to parse remote peer IP");
        return;
    }

    SDL_strlcpy(local_peer.uuid, "2f8a34f8-88ec-4047-a95e-cce96b122107", sizeof(local_peer.uuid));
    SDL_strlcpy(remote_peer.uuid, "8b4d717f-5d56-44dd-a07a-68de8e1617f7", sizeof(remote_peer.uuid));

    const ChessRole role = chess_elect_role(&local_peer, &remote_peer);
    if (role == CHESS_ROLE_SERVER) {
        SDL_Log("LAN election example: local peer is SERVER");
    } else if (role == CHESS_ROLE_CLIENT) {
        SDL_Log("LAN election example: local peer is CLIENT");
    } else {
        SDL_Log("LAN election example: role is UNKNOWN");
    }
}

static ChessNetworkSession build_demo_network_session(void)
{
    ChessPeerInfo local_peer;
    ChessPeerInfo remote_peer;
    ChessNetworkSession session;

    memset(&local_peer, 0, sizeof(local_peer));
    memset(&remote_peer, 0, sizeof(remote_peer));

    chess_parse_ipv4("192.168.1.12", &local_peer.ipv4_host_order);
    chess_parse_ipv4("192.168.1.48", &remote_peer.ipv4_host_order);
    SDL_strlcpy(local_peer.uuid, "2f8a34f8-88ec-4047-a95e-cce96b122107", sizeof(local_peer.uuid));
    SDL_strlcpy(remote_peer.uuid, "8b4d717f-5d56-44dd-a07a-68de8e1617f7", sizeof(remote_peer.uuid));

    chess_network_session_init(&session, &local_peer);
    chess_network_session_set_remote(&session, &remote_peer);
    return session;
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

    log_host_election_example();

    ChessNetworkSession network_session = build_demo_network_session();

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        chess_network_session_step(&network_session);

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_board(renderer, width, height);

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
