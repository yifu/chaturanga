#ifndef CHESS_APP_NETWORK_PEER_H
#define CHESS_APP_NETWORK_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHESS_UUID_STRING_LEN 37
#define CHESS_PEER_USERNAME_MAX_LEN 64
#define CHESS_PEER_HOSTNAME_MAX_LEN 64

typedef struct ChessPeerInfo {
    uint32_t ipv4_host_order;
    char uuid[CHESS_UUID_STRING_LEN];
    char username[CHESS_PEER_USERNAME_MAX_LEN];
    char hostname[CHESS_PEER_HOSTNAME_MAX_LEN];
} ChessPeerInfo;

typedef enum ChessRole {
    CHESS_ROLE_UNKNOWN = 0,
    CHESS_ROLE_SERVER,
    CHESS_ROLE_CLIENT
} ChessRole;

bool chess_parse_ipv4(const char *ip_str, uint32_t *out_ipv4_host_order);
bool chess_generate_peer_uuid(char *out_uuid, size_t out_uuid_size);
void chess_peer_set_identity_tokens(ChessPeerInfo *peer, const char *username, const char *hostname);
bool chess_peer_init_local_identity(ChessPeerInfo *peer);
ChessRole chess_elect_role(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer);

#endif
