#include "chess_app/network_peer.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void copy_sanitized_token(
    char *dst,
    size_t dst_size,
    const char *src,
    const char *fallback)
{
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src || src[0] == '\0') {
        src = fallback;
    }
    if (!src || src[0] == '\0') {
        src = "unknown";
    }

    while (src[i] != '\0' && i + 1 < dst_size) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            dst[i] = (char)c;
        } else {
            dst[i] = '_';
        }
        ++i;
    }

    dst[i] = '\0';
    if (dst[0] == '\0') {
        (void)snprintf(dst, dst_size, "%s", "unknown");
    }
}

void chess_peer_set_identity_tokens(ChessPeerInfo *peer, const char *username, const char *hostname)
{
    if (!peer) {
        return;
    }

    copy_sanitized_token(peer->username, sizeof(peer->username), username, "player");
    copy_sanitized_token(peer->hostname, sizeof(peer->hostname), hostname, "host");
}

bool chess_peer_init_local_identity(ChessPeerInfo *peer)
{
    const char *env_username;
    const char *env_hostname;
    const char *login_name;
    char hostname_buf[CHESS_PEER_HOSTNAME_MAX_LEN];
    char *dot;

    if (!peer) {
        return false;
    }

    memset(peer, 0, sizeof(*peer));

    if (!chess_generate_peer_uuid(peer->uuid, sizeof(peer->uuid))) {
        return false;
    }

    env_username = getenv("CHESS_USERNAME");
    env_hostname = getenv("CHESS_HOSTNAME");
    login_name = getlogin();

    memset(hostname_buf, 0, sizeof(hostname_buf));
    if (env_hostname && env_hostname[0] != '\0') {
        (void)snprintf(hostname_buf, sizeof(hostname_buf), "%s", env_hostname);
    } else if (gethostname(hostname_buf, sizeof(hostname_buf) - 1) != 0 || hostname_buf[0] == '\0') {
        (void)snprintf(hostname_buf, sizeof(hostname_buf), "%s", "host");
    }

    dot = strchr(hostname_buf, '.');
    if (dot) {
        *dot = '\0';
    }

    {
        const char *chosen_user = (env_username && env_username[0] != '\0') ? env_username
                                : getenv("USER") ? getenv("USER")
                                : login_name ? login_name
                                : NULL;
        chess_peer_set_identity_tokens(peer, chosen_user, hostname_buf);
    }
    return true;
}

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
    srand((unsigned int)(time(NULL) ^ getpid()));
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
