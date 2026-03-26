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
    CHESS_CHALLENGE_MATCHED,
    CHESS_CHALLENGE_CONNECT_FAILED
} ChessChallengeState;

/* Per-peer outgoing challenge connection state.
 * Each challenged peer gets its own TCP fd and handshake flags
 * so that multiple challenges can be in flight simultaneously. */
typedef struct ChessChallengeConnection {
    int fd;                         /* TCP socket, -1 if not connected         */
    bool connect_attempted;
    bool hello_sent;
    bool hello_received;
    bool hello_completed;
    uint64_t next_connect_attempt_at;
    unsigned int connect_failures;  /* consecutive TCP connect failures        */
} ChessChallengeConnection;

typedef struct ChessDiscoveredPeerState {
    ChessPeerInfo peer;
    uint32_t tcp_ipv4;              /* remote IP (host order) for TCP connect   */
    uint16_t tcp_port;
    ChessChallengeState challenge_state;
    uint64_t discovered_at_ms;
    bool offer_sent;
    ChessChallengeConnection challenge_conn;
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
void chess_lobby_close_all_challenge_connections(ChessLobbyState *lobby);
void chess_lobby_close_challenge_connection(ChessLobbyState *lobby, int peer_idx);

#endif
