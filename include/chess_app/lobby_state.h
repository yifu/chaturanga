#ifndef CHESS_APP_LOBBY_STATE_H
#define CHESS_APP_LOBBY_STATE_H

#include "chess_app/network_peer.h"

#include <stdbool.h>
#include <stdint.h>

#define CHESS_MAX_DISCOVERED_PEERS 16

typedef enum ChessChallengeState {
    CHESS_CHALLENGE_NONE = 0,
    CHESS_CHALLENGE_OUTGOING_PENDING,
    CHESS_CHALLENGE_INCOMING_PENDING,
    CHESS_CHALLENGE_MATCHED
} ChessChallengeState;

typedef struct ChessDiscoveredPeerState {
    ChessPeerInfo peer;
    uint32_t tcp_ipv4;              /* remote IP (host order) for TCP connect   */
    uint16_t tcp_port;
    ChessChallengeState challenge_state;
    uint64_t discovered_at_ms;
    bool offer_sent;
} ChessDiscoveredPeerState;

typedef struct ChessLobbyState {
    ChessDiscoveredPeerState discovered_peers[CHESS_MAX_DISCOVERED_PEERS];
    int discovered_peer_count;
    int selected_peer_idx;
    int hovered_peer_idx;
} ChessLobbyState;

void chess_lobby_init(ChessLobbyState *lobby);
void chess_lobby_add_or_update_peer(
    ChessLobbyState *lobby,
    const ChessPeerInfo *peer,
    uint32_t tcp_ipv4,
    uint16_t tcp_port
);
bool chess_lobby_find_peer(
    const ChessLobbyState *lobby,
    const ChessPeerInfo *peer,
    int *out_idx
);
void chess_lobby_set_challenge_state(
    ChessLobbyState *lobby,
    int peer_idx,
    ChessChallengeState state
);
ChessChallengeState chess_lobby_get_challenge_state(
    const ChessLobbyState *lobby,
    int peer_idx
);
void chess_lobby_mark_offer_sent(ChessLobbyState *lobby, int peer_idx);
bool chess_lobby_has_offer_been_sent(
    const ChessLobbyState *lobby,
    int peer_idx
);
bool chess_lobby_remove_peer_by_profile_id(ChessLobbyState *lobby, const char *profile_id);

#endif
