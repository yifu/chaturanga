#include "chess_app/network_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool configure_connected_socket(int fd)
{
    int opt = 1;
#if defined(SO_NOSIGPIPE)
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt)) != 0) {
        return false;
    }
#endif
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) != 0) {
        return false;
    }
    return true;
}

static bool recv_all_with_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
    size_t received = 0;

    while (received < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        ssize_t n = 0;
        int pr;

        do {
            pr = poll(&pfd, 1, timeout_ms);
        } while (pr < 0 && errno == EINTR);
        if (pr <= 0) {
            return false;
        }

        n = recv(fd, (char *)buf + received, len - received, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }

        received += (size_t)n;
    }

    return true;
}

static bool send_all(int fd, const void *buf, size_t len)
{
    struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
    int pr;
    size_t sent = 0;

    do {
        pr = poll(&pfd, 1, 0);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        return false;
    }

    while (sent < len) {
        ssize_t n = 0;
#if defined(MSG_NOSIGNAL)
        n = send(fd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
#else
        n = send(fd, (const char *)buf + sent, len - sent, 0);
#endif
        if (n < 0 && errno == EINTR) {
            continue;
        }
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
    struct pollfd pfd;
    int client_fd = -1;

    if (!listener || listener->fd < 0 || !out_conn) {
        return false;
    }

    pfd.fd = listener->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, timeout_ms) <= 0) {
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

bool chess_tcp_connect_start(uint32_t remote_ipv4_host_order, uint16_t remote_port, int *out_fd)
{
    int fd;
    int flags;
    struct sockaddr_in addr;

    if (!out_fd) {
        return false;
    }

    *out_fd = -1;

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
        *out_fd = fd;
        return true;
    }

    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    *out_fd = fd;
    return true;
}

ChessConnectResult chess_tcp_connect_check(int fd)
{
    int err = 0;
    socklen_t err_len = sizeof(err);

    if (fd < 0) {
        return CHESS_CONNECT_FAILED;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0) {
        return CHESS_CONNECT_FAILED;
    }

    if (err == 0) {
        if (!configure_connected_socket(fd)) {
            return CHESS_CONNECT_FAILED;
        }
        return CHESS_CONNECT_CONNECTED;
    }

    if (err == EINPROGRESS || err == EALREADY) {
        return CHESS_CONNECT_IN_PROGRESS;
    }

    return CHESS_CONNECT_FAILED;
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

/* ── Non-blocking receive ───────────────────────────────────────────── */

bool chess_tcp_set_nonblocking(ChessTcpConnection *conn)
{
    int flags;

    if (!conn || conn->fd < 0) {
        return false;
    }

    flags = fcntl(conn->fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    return fcntl(conn->fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void chess_tcp_recv_reset(ChessTcpRecvBuffer *rb)
{
    if (!rb) {
        return;
    }

    rb->state = CHESS_RECV_WAITING_HEADER;
    rb->done = 0;
}

ChessRecvResult chess_tcp_recv_nonblocking(ChessTcpConnection *conn, ChessTcpRecvBuffer *rb,
                                           ChessPacketHeader *out_header,
                                           uint8_t *out_payload, size_t payload_capacity)
{
    ssize_t n;

    if (!conn || conn->fd < 0 || !rb || !out_header) {
        return CHESS_RECV_ERROR;
    }

    /* Phase 1: accumulate header bytes */
    if (rb->state == CHESS_RECV_WAITING_HEADER) {
        size_t need = sizeof(ChessPacketHeader) - rb->done;

        n = recv(conn->fd, rb->buf + rb->done, need, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return CHESS_RECV_INCOMPLETE;
            }
            return CHESS_RECV_ERROR;
        }
        if (n == 0) {
            return CHESS_RECV_ERROR;
        }

        rb->done += (size_t)n;
        if (rb->done < sizeof(ChessPacketHeader)) {
            return CHESS_RECV_INCOMPLETE;
        }

        /* Header complete — validate */
        {
            const ChessPacketHeader *hdr = (const ChessPacketHeader *)rb->buf;

            if (hdr->protocol_version != CHESS_PROTOCOL_VERSION) {
                return CHESS_RECV_ERROR;
            }
            if (hdr->payload_size > sizeof(rb->buf) - sizeof(ChessPacketHeader)) {
                return CHESS_RECV_ERROR;
            }
            if (hdr->payload_size == 0u) {
                memcpy(out_header, hdr, sizeof(*out_header));
                chess_tcp_recv_reset(rb);
                return CHESS_RECV_OK;
            }
            if (hdr->payload_size > payload_capacity) {
                return CHESS_RECV_ERROR;
            }
        }

        rb->state = CHESS_RECV_WAITING_PAYLOAD;
        /* fall through to payload phase */
    }

    /* Phase 2: accumulate payload bytes */
    {
        const ChessPacketHeader *hdr = (const ChessPacketHeader *)rb->buf;
        size_t payload_done = rb->done - sizeof(ChessPacketHeader);
        size_t need = hdr->payload_size - payload_done;

        n = recv(conn->fd, rb->buf + rb->done, need, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return CHESS_RECV_INCOMPLETE;
            }
            return CHESS_RECV_ERROR;
        }
        if (n == 0) {
            return CHESS_RECV_ERROR;
        }

        rb->done += (size_t)n;
        payload_done += (size_t)n;
        if (payload_done < hdr->payload_size) {
            return CHESS_RECV_INCOMPLETE;
        }

        /* Packet complete */
        memcpy(out_header, hdr, sizeof(*out_header));
        if (out_payload) {
            memcpy(out_payload, rb->buf + sizeof(ChessPacketHeader), hdr->payload_size);
        }
        chess_tcp_recv_reset(rb);
        return CHESS_RECV_OK;
    }
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

bool chess_tcp_send_offer(ChessTcpConnection *conn, const ChessOfferPayload *offer)
{
    if (!conn || conn->fd < 0 || !offer) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_OFFER, 1u, offer, (uint32_t)sizeof(*offer));
}

bool chess_tcp_send_accept(ChessTcpConnection *conn, const ChessAcceptPayload *accept)
{
    if (!conn || conn->fd < 0 || !accept) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_ACCEPT, 1u, accept, (uint32_t)sizeof(*accept));
}

bool chess_tcp_send_resume_request(ChessTcpConnection *conn, const ChessResumeRequestPayload *request)
{
    if (!conn || conn->fd < 0 || !request) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_RESUME_REQUEST, 3u, request, (uint32_t)sizeof(*request));
}

bool chess_tcp_send_resume_response(ChessTcpConnection *conn, const ChessResumeResponsePayload *response)
{
    if (!conn || conn->fd < 0 || !response) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_RESUME_RESPONSE, 3u, response, (uint32_t)sizeof(*response));
}

bool chess_tcp_send_state_snapshot(ChessTcpConnection *conn, const ChessStateSnapshotPayload *snapshot)
{
    if (!conn || conn->fd < 0 || !snapshot) {
        return false;
    }

    return chess_tcp_send_packet(conn, CHESS_MSG_STATE_SNAPSHOT, 4u, snapshot, (uint32_t)sizeof(*snapshot));
}

