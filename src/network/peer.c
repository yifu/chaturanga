#include "chess_app/network_peer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

bool chess_generate_peer_uuid(char *out_uuid, size_t out_uuid_size)
{
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;

    if (!out_uuid || out_uuid_size < CHESS_UUID_STRING_LEN) {
        return false;
    }

    srand((unsigned int)(time(NULL) ^ getpid()));
    a = (unsigned int)rand();
    b = (unsigned int)rand();
    c = (unsigned int)rand();
    d = (unsigned int)rand();

    (void)snprintf(
        out_uuid,
        out_uuid_size,
        "%08x-%04x-4%03x-a%03x-%08x%04x",
        a,
        b & 0xffffu,
        c & 0x0fffu,
        d & 0x0fffu,
        (a ^ c) & 0xffffffffu,
        (b ^ d) & 0xffffu
    );

    return true;
}

ChessRole chess_elect_role(const ChessPeerInfo *local_peer, const ChessPeerInfo *remote_peer)
{
    int cmp;

    if (!local_peer || !remote_peer) {
        return CHESS_ROLE_UNKNOWN;
    }

    /* Primary: compare IPs (from getifaddrs for local; from DNS-SD for remote —
     * loopback is substituted with the local LAN IP in addr_callback so both
     * sides see the same value when running on the same machine).
     * Fallback: UUID, which is always unique and symmetric. */
    if (local_peer->ipv4_host_order < remote_peer->ipv4_host_order) {
        return CHESS_ROLE_SERVER;
    }
    if (local_peer->ipv4_host_order > remote_peer->ipv4_host_order) {
        return CHESS_ROLE_CLIENT;
    }

    cmp = strncmp(local_peer->uuid, remote_peer->uuid, CHESS_UUID_STRING_LEN);
    if (cmp < 0) {
        return CHESS_ROLE_SERVER;
    }
    if (cmp > 0) {
        return CHESS_ROLE_CLIENT;
    }

    return CHESS_ROLE_UNKNOWN;
}
