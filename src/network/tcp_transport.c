#include "chess_app/tcp_transport.h"

static bool tcp_send_packet_impl(void *self, uint32_t message_type, uint32_t sequence,
                                  const void *payload, uint32_t payload_size) {
    TcpTransport *tcp = (TcpTransport *)self;
    return chess_tcp_send_packet(&tcp->connection, message_type, sequence, payload, payload_size);
}

static bool tcp_send_packet_pair_impl(void *self,
                                       uint32_t msg_type_1, uint32_t seq_1, const void *payload_1, uint32_t payload_size_1,
                                       uint32_t msg_type_2, uint32_t seq_2, const void *payload_2, uint32_t payload_size_2) {
    TcpTransport *tcp = (TcpTransport *)self;
    return chess_tcp_send_packet_pair(&tcp->connection,
        msg_type_1, seq_1, payload_1, payload_size_1,
        msg_type_2, seq_2, payload_2, payload_size_2);
}

static ChessRecvResult tcp_recv_nonblocking_impl(void *self,
                                                   ChessPacketHeader *out_header,
                                                   uint8_t *out_payload, size_t payload_capacity) {
    TcpTransport *tcp = (TcpTransport *)self;
    return chess_tcp_recv_nonblocking(&tcp->connection, &tcp->recv_buffer,
                                      out_header, out_payload, payload_capacity);
}

static void tcp_recv_reset_impl(void *self) {
    TcpTransport *tcp = (TcpTransport *)self;
    chess_tcp_recv_reset(&tcp->recv_buffer);
}

static void tcp_close_impl(void *self) {
    TcpTransport *tcp = (TcpTransport *)self;
    chess_tcp_connection_close(&tcp->connection);
}

static int tcp_get_fd_impl(const void *self) {
    const TcpTransport *tcp = (const TcpTransport *)self;
    return tcp->connection.fd;
}

static bool tcp_set_nonblocking_impl(void *self) {
    TcpTransport *tcp = (TcpTransport *)self;
    return chess_tcp_set_nonblocking(&tcp->connection);
}

const TransportOps tcp_transport_ops = {
    .send_packet      = tcp_send_packet_impl,
    .send_packet_pair = tcp_send_packet_pair_impl,
    .recv_nonblocking = tcp_recv_nonblocking_impl,
    .recv_reset       = tcp_recv_reset_impl,
    .close            = tcp_close_impl,
    .get_fd           = tcp_get_fd_impl,
    .set_nonblocking  = tcp_set_nonblocking_impl,
};

void tcp_transport_init(TcpTransport *tcp) {
    if (!tcp) return;
    tcp->base.ops = &tcp_transport_ops;
    tcp->connection.fd = -1;
    chess_tcp_recv_reset(&tcp->recv_buffer);
}

void tcp_transport_init_from_fd(TcpTransport *tcp, int fd) {
    if (!tcp) return;
    tcp->base.ops = &tcp_transport_ops;
    tcp->connection.fd = fd;
    chess_tcp_recv_reset(&tcp->recv_buffer);
}
