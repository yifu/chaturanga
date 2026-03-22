#include "chess_app/network_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool configure_connected_socket(int fd)
{
#if defined(SO_NOSIGPIPE)
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)) != 0) {
        return false;
    }
#endif
    return true;
}

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
        ssize_t n = 0;
#if defined(MSG_NOSIGNAL)
        n = send(fd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
#else
        n = send(fd, (const char *)buf + sent, len - sent, 0);
#endif
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

    if (!configure_connected_socket(client_fd)) {
        close(client_fd);
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
        if (!configure_connected_socket(fd)) {
            close(fd);
            return false;
        }
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

    if (!configure_connected_socket(fd)) {
        close(fd);
        return false;
    }

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

bool chess_tcp_send_packet(ChessTcpConnection *conn, uint32_t message_type, uint32_t sequence, const void *payload, uint32_t payload_size)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0) {
        return false;
    }

    header.protocol_version = CHESS_PROTOCOL_VERSION;
    header.message_type = message_type;
    header.sequence = sequence;
    header.payload_size = payload_size;

    if (!send_all(conn->fd, &header, sizeof(header))) {
        return false;
    }

    if (payload_size == 0u) {
        return true;
    }

    return payload != NULL && send_all(conn->fd, payload, payload_size);
}

bool chess_tcp_recv_packet_header(ChessTcpConnection *conn, int timeout_ms, ChessPacketHeader *out_header)
{
    if (!conn || conn->fd < 0 || !out_header) {
        return false;
    }

    if (!recv_all_with_timeout(conn->fd, out_header, sizeof(*out_header), timeout_ms)) {
        return false;
    }

    return out_header->protocol_version == CHESS_PROTOCOL_VERSION;
}

bool chess_tcp_recv_payload(ChessTcpConnection *conn, int timeout_ms, void *out_payload, uint32_t payload_size)
{
    if (!conn || conn->fd < 0) {
        return false;
    }

    if (payload_size == 0u) {
        return true;
    }

    if (!out_payload) {
        return false;
    }

    return recv_all_with_timeout(conn->fd, out_payload, payload_size, timeout_ms);
}

bool chess_tcp_send_hello(ChessTcpConnection *conn, const ChessHelloPayload *hello)
{
    if (!conn || conn->fd < 0 || !hello) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_HELLO, 1u, hello, (uint32_t)sizeof(*hello));
}

bool chess_tcp_recv_hello(ChessTcpConnection *conn, int timeout_ms, ChessHelloPayload *out_hello)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !out_hello) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(conn, timeout_ms, &header)) {
        return false;
    }

    if (header.protocol_version != CHESS_PROTOCOL_VERSION ||
        header.message_type != CHESS_MSG_HELLO ||
        header.payload_size != sizeof(ChessHelloPayload)) {
        return false;
    }

    return chess_tcp_recv_payload(conn, timeout_ms, out_hello, (uint32_t)sizeof(*out_hello));
}

bool chess_tcp_send_ack(ChessTcpConnection *conn, uint32_t acked_message_type, uint32_t acked_sequence, uint32_t status_code)
{
    ChessAckPayload ack;

    if (!conn || conn->fd < 0) {
        return false;
    }

    ack.acked_message_type = acked_message_type;
    ack.acked_sequence = acked_sequence;
    ack.status_code = status_code;
    return chess_tcp_send_packet(conn, CHESS_MSG_ACK, acked_sequence, &ack, (uint32_t)sizeof(ack));
}

bool chess_tcp_recv_ack(ChessTcpConnection *conn, int timeout_ms, ChessAckPayload *out_ack)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !out_ack) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(conn, timeout_ms, &header)) {
        return false;
    }

    if (header.message_type != CHESS_MSG_ACK ||
        header.payload_size != sizeof(*out_ack)) {
        return false;
    }

    return chess_tcp_recv_payload(conn, timeout_ms, out_ack, (uint32_t)sizeof(*out_ack));
}

bool chess_tcp_send_start(ChessTcpConnection *conn, const ChessStartPayload *start)
{
    if (!conn || conn->fd < 0 || !start) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_START, 2u, start, (uint32_t)sizeof(*start));
}

bool chess_tcp_recv_start(ChessTcpConnection *conn, int timeout_ms, ChessStartPayload *out_start)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !out_start) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(conn, timeout_ms, &header)) {
        return false;
    }

    if (header.message_type != CHESS_MSG_START ||
        header.payload_size != sizeof(*out_start)) {
        return false;
    }

    return chess_tcp_recv_payload(conn, timeout_ms, out_start, (uint32_t)sizeof(*out_start));
}

bool chess_tcp_send_offer(ChessTcpConnection *conn, const ChessOfferPayload *offer)
{
    if (!conn || conn->fd < 0 || !offer) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_OFFER, 1u, offer, (uint32_t)sizeof(*offer));
}

bool chess_tcp_recv_offer(ChessTcpConnection *conn, int timeout_ms, ChessOfferPayload *out_offer)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !out_offer) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(conn, timeout_ms, &header)) {
        return false;
    }

    if (header.message_type != CHESS_MSG_OFFER ||
        header.payload_size != sizeof(*out_offer)) {
        return false;
    }

    return chess_tcp_recv_payload(conn, timeout_ms, out_offer, (uint32_t)sizeof(*out_offer));
}

bool chess_tcp_send_accept(ChessTcpConnection *conn, const ChessAcceptPayload *accept)
{
    if (!conn || conn->fd < 0 || !accept) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_ACCEPT, 1u, accept, (uint32_t)sizeof(*accept));
}

bool chess_tcp_recv_accept(ChessTcpConnection *conn, int timeout_ms, ChessAcceptPayload *out_accept)
{
    ChessPacketHeader header;

    if (!conn || conn->fd < 0 || !out_accept) {
        return false;
    }

    if (!chess_tcp_recv_packet_header(conn, timeout_ms, &header)) {
        return false;
    }

    if (header.message_type != CHESS_MSG_ACCEPT ||
        header.payload_size != sizeof(*out_accept)) {
        return false;
    }

    return chess_tcp_recv_payload(conn, timeout_ms, out_accept, (uint32_t)sizeof(*out_accept));
}
