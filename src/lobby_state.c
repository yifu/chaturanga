#include "chess_app/lobby_state.h"

#include <SDL3/SDL.h>
#include <string.h>

void chess_lobby_init(ChessLobbyState *lobby)
{
    if (!lobby) {
        return;
    }

    memset(lobby, 0, sizeof(*lobby));
    lobby->discovered_peer_count = 0;
    lobby->selected_peer_idx = -1;
}

void chess_lobby_add_or_update_peer(
    ChessLobbyState *lobby,
    const ChessPeerInfo *peer,
    uint16_t tcp_port)
{
    int idx = -1;
    int i;

    if (!lobby || !peer) {
        return;
    }

    /* Find existing peer by UUID */
    for (i = 0; i < lobby->discovered_peer_count; ++i) {
        if (SDL_strcmp(lobby->discovered_peers[i].peer.uuid, peer->uuid) == 0) {
            idx = i;
            break;
        }
    }

    /* Add new peer if not found and space available */
    if (idx == -1) {
        if (lobby->discovered_peer_count < CHESS_MAX_DISCOVERED_PEERS) {
            idx = lobby->discovered_peer_count;
            lobby->discovered_peer_count++;
        } else {
            SDL_Log("Lobby is full, cannot add new peer");
            return;
        }
    }

    /* Update peer info */
    lobby->discovered_peers[idx].peer = *peer;
    lobby->discovered_peers[idx].tcp_port = tcp_port;
    lobby->discovered_peers[idx].discovered_at_ms = SDL_GetTicks();

    /* Preserve existing challenge state if updating */
    if (idx >= 0) {
        if (lobby->discovered_peers[idx].challenge_state == CHESS_CHALLENGE_NONE &&
            SDL_strcmp(lobby->discovered_peers[idx].peer.uuid, peer->uuid) == 0) {
            /* Already set, don't override */
        }
    }
}

bool chess_lobby_find_peer(
    const ChessLobbyState *lobby,
    const ChessPeerInfo *peer,
    int *out_idx)
{
    int i;

    if (!lobby || !peer || !out_idx) {
        return false;
    }

    for (i = 0; i < lobby->discovered_peer_count; ++i) {
        if (SDL_strcmp(lobby->discovered_peers[i].peer.uuid, peer->uuid) == 0) {
            *out_idx = i;
            return true;
        }
    }

    *out_idx = -1;
    return false;
}

void chess_lobby_set_challenge_state(
    ChessLobbyState *lobby,
    int peer_idx,
    ChessChallengeState state)
{
    if (!lobby || peer_idx < 0 || peer_idx >= lobby->discovered_peer_count) {
        return;
    }

    lobby->discovered_peers[peer_idx].challenge_state = state;
}

ChessChallengeState chess_lobby_get_challenge_state(
    const ChessLobbyState *lobby,
    int peer_idx)
{
    if (!lobby || peer_idx < 0 || peer_idx >= lobby->discovered_peer_count) {
        return CHESS_CHALLENGE_NONE;
    }

    return lobby->discovered_peers[peer_idx].challenge_state;
}
