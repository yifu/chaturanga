#include "chess_app/network_tcp.h"

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
