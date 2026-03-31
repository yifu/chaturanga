#include "chess_app/network_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
    size_t sent = 0;

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
    struct iovec iov[2];
    int iovcnt = 1;

    if (!conn || conn->fd < 0) {
        return false;
    }

    header.protocol_version = CHESS_PROTOCOL_VERSION;
    header.message_type = message_type;
    header.sequence = sequence;
    header.payload_size = payload_size;

    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);

    if (payload_size > 0u && payload) {
        iov[1].iov_base = (void *)(uintptr_t)payload;
        iov[1].iov_len = payload_size;
        iovcnt = 2;
    } else if (payload_size > 0u) {
        return false;
    }

    {
        size_t total = iov[0].iov_len + (iovcnt > 1 ? iov[1].iov_len : 0u);
        size_t sent = 0;

        while (sent < total) {
            ssize_t n = writev(conn->fd, iov, iovcnt);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return false;
            }
            sent += (size_t)n;
            /* Advance iov past bytes already sent */
            while (iovcnt > 0 && (size_t)n >= iov[0].iov_len) {
                n -= (ssize_t)iov[0].iov_len;
                iov[0] = iov[1];
                iovcnt--;
            }
            if (iovcnt > 0 && n > 0) {
                iov[0].iov_base = (char *)iov[0].iov_base + n;
                iov[0].iov_len -= (size_t)n;
            }
        }
    }

    return true;
}

bool chess_tcp_send_packet_pair(
    ChessTcpConnection *conn,
    uint32_t msg_type_1, uint32_t seq_1, const void *payload_1, uint32_t payload_size_1,
    uint32_t msg_type_2, uint32_t seq_2, const void *payload_2, uint32_t payload_size_2)
{
    ChessPacketHeader hdr1;
    ChessPacketHeader hdr2;
    struct iovec iov[4];
    int iovcnt = 0;
    size_t total;
    size_t sent;

    if (!conn || conn->fd < 0) {
        return false;
    }

    hdr1.protocol_version = CHESS_PROTOCOL_VERSION;
    hdr1.message_type = msg_type_1;
    hdr1.sequence = seq_1;
    hdr1.payload_size = payload_size_1;

    iov[iovcnt].iov_base = &hdr1;
    iov[iovcnt].iov_len = sizeof(hdr1);
    iovcnt++;

    if (payload_size_1 > 0u && payload_1) {
        iov[iovcnt].iov_base = (void *)(uintptr_t)payload_1;
        iov[iovcnt].iov_len = payload_size_1;
        iovcnt++;
    }

    hdr2.protocol_version = CHESS_PROTOCOL_VERSION;
    hdr2.message_type = msg_type_2;
    hdr2.sequence = seq_2;
    hdr2.payload_size = payload_size_2;

    iov[iovcnt].iov_base = &hdr2;
    iov[iovcnt].iov_len = sizeof(hdr2);
    iovcnt++;

    if (payload_size_2 > 0u && payload_2) {
        iov[iovcnt].iov_base = (void *)(uintptr_t)payload_2;
        iov[iovcnt].iov_len = payload_size_2;
        iovcnt++;
    }

    total = 0;
    {
        int i;
        for (i = 0; i < iovcnt; ++i) {
            total += iov[i].iov_len;
        }
    }

    sent = 0;

    while (sent < total) {
        ssize_t n = writev(conn->fd, iov, iovcnt);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
        while (iovcnt > 0 && (size_t)n >= iov[0].iov_len) {
            n -= (ssize_t)iov[0].iov_len;
            memmove(&iov[0], &iov[1], (size_t)(iovcnt - 1) * sizeof(iov[0]));
            iovcnt--;
        }
        if (iovcnt > 0 && n > 0) {
            iov[0].iov_base = (char *)iov[0].iov_base + n;
            iov[0].iov_len -= (size_t)n;
        }
    }

    return true;
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

