#include "chess_app/network_peer.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <string.h>

bool chess_parse_ipv4(const char *ip_str, uint32_t *out_ipv4_host_order)
{
    struct in_addr addr;

    if (!ip_str || !out_ipv4_host_order) {
        return false;
    }

    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        return false;
    }

    *out_ipv4_host_order = ntohl(addr.s_addr);
    return true;
}

ChessRole chess_elect_role(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer)
{
    if (!local_peer || !remote_peer) {
        return CHESS_ROLE_UNKNOWN;
    }

    if (local_peer->ipv4_host_order < remote_peer->ipv4_host_order) {
        return CHESS_ROLE_SERVER;
    }

    if (local_peer->ipv4_host_order > remote_peer->ipv4_host_order) {
        return CHESS_ROLE_CLIENT;
    }

    if (strncmp(local_peer->uuid, remote_peer->uuid, CHESS_UUID_STRING_LEN) < 0) {
        return CHESS_ROLE_SERVER;
    }

    if (strncmp(local_peer->uuid, remote_peer->uuid, CHESS_UUID_STRING_LEN) > 0) {
        return CHESS_ROLE_CLIENT;
    }

    return CHESS_ROLE_UNKNOWN;
}
