#ifndef CHESS_APP_NETWORK_PEER_H
#define CHESS_APP_NETWORK_PEER_H

#include <stdbool.h>
#include <stdint.h>

#define CHESS_UUID_STRING_LEN 37

typedef struct ChessPeerInfo {
    uint32_t ipv4_host_order;
    char uuid[CHESS_UUID_STRING_LEN];
} ChessPeerInfo;

typedef enum ChessRole {
    CHESS_ROLE_UNKNOWN = 0,
    CHESS_ROLE_SERVER,
    CHESS_ROLE_CLIENT
} ChessRole;

bool chess_parse_ipv4(const char *ip_str, uint32_t *out_ipv4_host_order);
ChessRole chess_elect_role(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer);

#endif
