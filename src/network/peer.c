#include "chess_app/network_peer.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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

bool chess_get_default_local_ipv4(uint32_t *out_ipv4_host_order)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    bool found = false;

    if (!out_ipv4_host_order) {
        return false;
    }

    if (getifaddrs(&ifaddr) != 0) {
        return false;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        *out_ipv4_host_order = ntohl(sin->sin_addr.s_addr);
        found = true;
        break;
    }

    freeifaddrs(ifaddr);
    return found;
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
