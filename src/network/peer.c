#include "chess_app/network_peer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
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

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    a = arc4random();
    b = arc4random();
    c = arc4random();
    d = arc4random();
#else
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)(time(NULL) ^ getpid()));
        seeded = 1;
    }
    a = (unsigned int)rand();
    b = (unsigned int)rand();
    c = (unsigned int)rand();
    d = (unsigned int)rand();
#endif

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
