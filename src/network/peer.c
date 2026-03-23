#include "chess_app/network_peer.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHESS_PROFILE_FILENAME "profile.json"

static bool build_profile_base_dir(char *out_dir, size_t out_dir_size)
{
    const char *override_dir;
    const char *home;
    const char *xdg_data_home;

    if (!out_dir || out_dir_size == 0) {
        return false;
    }

    override_dir = getenv("CHESS_APP_PROFILE_DIR");
    if (override_dir && override_dir[0] != '\0') {
        (void)snprintf(out_dir, out_dir_size, "%s", override_dir);
        return true;
    }

    home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return false;
    }

#if defined(__APPLE__)
    (void)snprintf(
        out_dir,
        out_dir_size,
        "%s/Library/Application Support/chess_app",
        home);
    return true;
#else
    xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && xdg_data_home[0] != '\0') {
        (void)snprintf(out_dir, out_dir_size, "%s/chess_app", xdg_data_home);
        return true;
    }
    (void)snprintf(out_dir, out_dir_size, "%s/.local/share/chess_app", home);
    return true;
#endif
}

static bool ensure_directory_recursive(const char *path)
{
    char tmp[PATH_MAX];
    char *p;
    size_t len;

    if (!path || path[0] == '\0') {
        return false;
    }

    (void)snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) {
        return false;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool build_profile_file_path(char *out_path, size_t out_path_size)
{
    char base_dir[PATH_MAX];

    if (!out_path || out_path_size == 0) {
        return false;
    }

    if (!build_profile_base_dir(base_dir, sizeof(base_dir))) {
        return false;
    }
    if (!ensure_directory_recursive(base_dir)) {
        return false;
    }

    (void)snprintf(out_path, out_path_size, "%s/%s", base_dir, CHESS_PROFILE_FILENAME);
    return true;
}

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[80];
    const char *pos;
    const char *start;
    const char *end;
    size_t len;

    if (!json || !key || !out || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    pos = strstr(json, pattern);
    if (!pos) {
        return false;
    }

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') {
        ++pos;
    }
    if (*pos != ':') {
        return false;
    }
    ++pos;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') {
        ++pos;
    }
    if (*pos != '"') {
        return false;
    }

    start = pos + 1;
    end = strchr(start, '"');
    if (!end) {
        return false;
    }

    len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool load_persistent_profile(
    const char *profile_path,
    char *out_profile_id,
    size_t out_profile_id_size,
    char *out_username,
    size_t out_username_size,
    char *out_hostname,
    size_t out_hostname_size)
{
    FILE *fp;
    char buf[1024];
    size_t nread;

    if (!profile_path || !out_profile_id || out_profile_id_size == 0) {
        return false;
    }

    out_profile_id[0] = '\0';
    if (out_username && out_username_size > 0) {
        out_username[0] = '\0';
    }
    if (out_hostname && out_hostname_size > 0) {
        out_hostname[0] = '\0';
    }

    fp = fopen(profile_path, "rb");
    if (!fp) {
        return false;
    }

    nread = fread(buf, 1, sizeof(buf) - 1, fp);
    (void)fclose(fp);
    buf[nread] = '\0';

    if (!extract_json_string(buf, "profile_id", out_profile_id, out_profile_id_size)) {
        return false;
    }
    if (out_username && out_username_size > 0) {
        (void)extract_json_string(buf, "username", out_username, out_username_size);
    }
    if (out_hostname && out_hostname_size > 0) {
        (void)extract_json_string(buf, "hostname", out_hostname, out_hostname_size);
    }
    return true;
}

static bool save_persistent_profile(
    const char *profile_path,
    const char *profile_id,
    const char *username,
    const char *hostname)
{
    char tmp_path[PATH_MAX];
    FILE *fp;

    if (!profile_path || !profile_id || !username || !hostname) {
        return false;
    }

    (void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", profile_path);
    fp = fopen(tmp_path, "wb");
    if (!fp) {
        return false;
    }

    (void)fprintf(
        fp,
        "{\n"
        "  \"version\": 1,\n"
        "  \"profile_id\": \"%s\",\n"
        "  \"username\": \"%s\",\n"
        "  \"hostname\": \"%s\"\n"
        "}\n",
        profile_id,
        username,
        hostname);
    if (fclose(fp) != 0) {
        return false;
    }

    if (rename(tmp_path, profile_path) != 0) {
        return false;
    }
    return true;
}

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
    char profile_path[PATH_MAX];
    char persisted_profile_id[CHESS_PROFILE_ID_STRING_LEN];
    char persisted_username[CHESS_PEER_USERNAME_MAX_LEN];
    char persisted_hostname[CHESS_PEER_HOSTNAME_MAX_LEN];
    char chosen_username[CHESS_PEER_USERNAME_MAX_LEN];
    char chosen_hostname[CHESS_PEER_HOSTNAME_MAX_LEN];
    char *dot;

    if (!peer) {
        return false;
    }

    memset(peer, 0, sizeof(*peer));

    persisted_profile_id[0] = '\0';
    persisted_username[0] = '\0';
    persisted_hostname[0] = '\0';
    chosen_username[0] = '\0';
    chosen_hostname[0] = '\0';

    if (build_profile_file_path(profile_path, sizeof(profile_path))) {
        (void)load_persistent_profile(
            profile_path,
            persisted_profile_id,
            sizeof(persisted_profile_id),
            persisted_username,
            sizeof(persisted_username),
            persisted_hostname,
            sizeof(persisted_hostname));
    } else {
        profile_path[0] = '\0';
    }

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

        if ((!chosen_user || chosen_user[0] == '\0') && persisted_username[0] != '\0') {
            chosen_user = persisted_username;
        }
        if ((!hostname_buf[0] || hostname_buf[0] == '\0') && persisted_hostname[0] != '\0') {
            (void)snprintf(hostname_buf, sizeof(hostname_buf), "%s", persisted_hostname);
        }

        copy_sanitized_token(chosen_username, sizeof(chosen_username), chosen_user, "player");
        copy_sanitized_token(chosen_hostname, sizeof(chosen_hostname), hostname_buf, "host");
        chess_peer_set_identity_tokens(peer, chosen_username, chosen_hostname);
    }

    if (persisted_profile_id[0] != '\0') {
        (void)snprintf(peer->profile_id, sizeof(peer->profile_id), "%s", persisted_profile_id);
    } else if (!chess_generate_peer_uuid(peer->profile_id, sizeof(peer->profile_id))) {
        return false;
    }

    if (profile_path[0] != '\0') {
        (void)save_persistent_profile(
            profile_path,
            peer->profile_id,
            peer->username,
            peer->hostname);
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
