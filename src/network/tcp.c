#include "chess_app/network_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool recv_all_with_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
    size_t received = 0;

    while (received < len) {
        fd_set rfds;
        struct timeval tv;
        int sel = 0;
        ssize_t n = 0;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            return false;
        }

        n = recv(fd, (char *)buf + received, len - received, 0);
        if (n <= 0) {
            return false;
        }

        received += (size_t)n;
    }

    return true;
}

static bool send_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }

        sent += (size_t)n;
    }

    return true;
}

bool chess_tcp_listener_open(ChessTcpListener *listener, uint16_t requested_port)
{
    int fd = -1;
    struct sockaddr_in addr;
    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);

    if (!listener) {
        return false;
    }

    listener->fd = -1;
    listener->port = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(requested_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    if (listen(fd, 8) != 0) {
        close(fd);
        return false;
    }

    memset(&bound_addr, 0, sizeof(bound_addr));
    if (getsockname(fd, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
        close(fd);
        return false;
    }

    listener->fd = fd;
    listener->port = ntohs(bound_addr.sin_port);
    return true;
}

void chess_tcp_listener_close(ChessTcpListener *listener)
{
    if (!listener) {
        return;
    }

    if (listener->fd >= 0) {
        close(listener->fd);
    }

    listener->fd = -1;
    listener->port = 0;
}

bool chess_tcp_accept_once(ChessTcpListener *listener, int timeout_ms, ChessTcpConnection *out_conn)
{
    fd_set rfds;
    struct timeval tv;
    int sel = 0;
    int client_fd = -1;

    if (!listener || listener->fd < 0 || !out_conn) {
        return false;
    }

    FD_ZERO(&rfds);
    FD_SET(listener->fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    sel = select(listener->fd + 1, &rfds, NULL, NULL, &tv);
    if (sel <= 0) {
        return false;
    }

    client_fd = accept(listener->fd, NULL, NULL);
    if (client_fd < 0) {
        return false;
    }

    out_conn->fd = client_fd;
    return true;
}

bool chess_tcp_connect_once(uint32_t remote_ipv4_host_order, uint16_t remote_port, int timeout_ms, ChessTcpConnection *out_conn)
{
    int fd = -1;
    int flags = 0;
    struct sockaddr_in addr;
    fd_set wfds;
    struct timeval tv;
    int sel = 0;
    int err = 0;
    socklen_t err_len = sizeof(err);

    if (!out_conn) {
        return false;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote_port);
    addr.sin_addr.s_addr = htonl(remote_ipv4_host_order);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        (void)fcntl(fd, F_SETFL, flags);
        out_conn->fd = fd;
        return true;
    }

    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    sel = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (sel <= 0) {
        close(fd);
        return false;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
        close(fd);
        return false;
    }

    (void)fcntl(fd, F_SETFL, flags);

    out_conn->fd = fd;
    return true;
}

void chess_tcp_connection_close(ChessTcpConnection *conn)
{
    if (!conn) {
        return;
    }

    if (conn->fd >= 0) {
        close(conn->fd);
    }

    conn->fd = -1;
}

bool chess_tcp_send_hello(ChessTcpConnection *conn, const ChessHelloPayload *hello)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !hello) {
        return false;
    }

    header.protocol_version = CHESS_PROTOCOL_VERSION;
    header.message_type = CHESS_MSG_HELLO;
    header.sequence = 1u;
    header.payload_size = (uint32_t)sizeof(ChessHelloPayload);

    return send_all(conn->fd, &header, sizeof(header)) && send_all(conn->fd, hello, sizeof(*hello));
}

bool chess_tcp_recv_hello(ChessTcpConnection *conn, int timeout_ms, ChessHelloPayload *out_hello)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !out_hello) {
        return false;
    }

    if (!recv_all_with_timeout(conn->fd, &header, sizeof(header), timeout_ms)) {
        return false;
    }

    if (header.protocol_version != CHESS_PROTOCOL_VERSION ||
        header.message_type != CHESS_MSG_HELLO ||
        header.payload_size != sizeof(ChessHelloPayload)) {
        return false;
    }

    return recv_all_with_timeout(conn->fd, out_hello, sizeof(*out_hello), timeout_ms);
}
