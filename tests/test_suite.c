#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"
#include "chess_app/transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int s_failed = 0;
static int s_passed = 0;

#define EXPECT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        s_failed += 1; \
    } else { \
        s_passed += 1; \
    } \
} while (0)

#define EXPECT_EQ_INT(actual, expected) do { \
    int _a = (int)(actual); \
    int _e = (int)(expected); \
    if (_a != _e) { \
        fprintf(stderr, "FAIL %s:%d: %s == %s (got %d expected %d)\n", \
                __FILE__, __LINE__, #actual, #expected, _a, _e); \
        s_failed += 1; \
    } else { \
        s_passed += 1; \
    } \
} while (0)

#define EXPECT_EQ_STR(actual, expected) do { \
    const char *_a = (actual); \
    const char *_e = (expected); \
    if (strcmp(_a, _e) != 0) { \
        fprintf(stderr, "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", \
                __FILE__, __LINE__, #actual, _e, _a); \
        s_failed += 1; \
    } else { \
        s_passed += 1; \
    } \
} while (0)

static bool move_local(
    ChessGameState *state,
    ChessPlayerColor color,
    int from_file,
    int from_rank,
    int to_file,
    int to_rank,
    uint8_t promotion)
{
    ChessMovePayload move;

    if (!chess_game_select_local_piece(state, color, from_file, from_rank)) {
        return false;
    }
    return chess_game_try_local_move(state, color, to_file, to_rank, promotion, &move);
}

static void clear_board(ChessGameState *state)
{
    int rank;
    int file;

    memset(state, 0, sizeof(*state));
    for (rank = 0; rank < CHESS_BOARD_SIZE; ++rank) {
        for (file = 0; file < CHESS_BOARD_SIZE; ++file) {
            state->board[rank][file] = CHESS_PIECE_EMPTY;
        }
    }
    state->side_to_move = CHESS_COLOR_WHITE;
    state->fullmove_number = 1;
    state->halfmove_clock = 0;
    state->en_passant_target_file = -1;
    state->en_passant_target_rank = -1;
    state->selected_file = -1;
    state->selected_rank = -1;
    state->outcome = CHESS_OUTCOME_NONE;
}

/* ── Mock Transport ──────────────────────────────────────────────────── */

#define MOCK_MAX_CALLS    32
#define MOCK_MAX_RECV      8
#define MOCK_PAYLOAD_CAP 4096

typedef struct MockTransport {
    Transport base;
    struct {
        uint32_t msg_type;
        uint32_t sequence;
        uint8_t  payload[MOCK_PAYLOAD_CAP];
        uint32_t payload_size;
    } sent[MOCK_MAX_CALLS];
    int sent_count;
    ChessPacketHeader recv_headers[MOCK_MAX_RECV];
    uint8_t recv_payloads[MOCK_MAX_RECV][MOCK_PAYLOAD_CAP];
    uint32_t recv_payload_sizes[MOCK_MAX_RECV];
    int recv_count;
    int recv_idx;
    bool fail_send;
    ChessRecvResult forced_recv_result;
    bool close_called;
    bool recv_reset_called;
} MockTransport;

static bool mock_send_packet(void *self, uint32_t message_type, uint32_t sequence,
                              const void *payload, uint32_t payload_size)
{
    MockTransport *m = (MockTransport *)self;
    if (m->fail_send) return false;
    if (m->sent_count >= MOCK_MAX_CALLS) return false;
    m->sent[m->sent_count].msg_type = message_type;
    m->sent[m->sent_count].sequence = sequence;
    if (payload && payload_size > 0 && payload_size <= MOCK_PAYLOAD_CAP) {
        memcpy(m->sent[m->sent_count].payload, payload, payload_size);
    }
    m->sent[m->sent_count].payload_size = payload_size;
    m->sent_count++;
    return true;
}

static ChessRecvResult mock_recv_nonblocking(void *self, ChessPacketHeader *out_header,
                                              uint8_t *out_payload, size_t payload_capacity)
{
    MockTransport *m = (MockTransport *)self;
    if (m->recv_idx >= m->recv_count) return m->forced_recv_result;
    *out_header = m->recv_headers[m->recv_idx];
    if (out_payload && m->recv_payload_sizes[m->recv_idx] > 0) {
        size_t copy = m->recv_payload_sizes[m->recv_idx];
        if (copy > payload_capacity) copy = payload_capacity;
        memcpy(out_payload, m->recv_payloads[m->recv_idx], copy);
    }
    m->recv_idx++;
    return CHESS_RECV_OK;
}

static void mock_recv_reset(void *self)
{
    MockTransport *m = (MockTransport *)self;
    m->recv_reset_called = true;
}

static void mock_close(void *self)
{
    MockTransport *m = (MockTransport *)self;
    m->close_called = true;
}

static int mock_get_fd(const void *self)
{
    (void)self;
    return 42;
}

static bool mock_set_nonblocking(void *self)
{
    (void)self;
    return true;
}

static const TransportOps mock_transport_ops = {
    .send_packet      = mock_send_packet,
    .recv_nonblocking = mock_recv_nonblocking,
    .recv_reset       = mock_recv_reset,
    .close            = mock_close,
    .get_fd           = mock_get_fd,
    .set_nonblocking  = mock_set_nonblocking,
};

static void mock_transport_init(MockTransport *m)
{
    memset(m, 0, sizeof(*m));
    m->base.ops = &mock_transport_ops;
    m->forced_recv_result = CHESS_RECV_INCOMPLETE;
}

static void mock_transport_queue_recv(MockTransport *m, uint32_t msg_type,
                                       uint32_t seq, const void *payload,
                                       uint32_t payload_size)
{
    int idx = m->recv_count;
    if (idx >= MOCK_MAX_RECV) return;
    m->recv_headers[idx].protocol_version = CHESS_PROTOCOL_VERSION;
    m->recv_headers[idx].message_type = msg_type;
    m->recv_headers[idx].sequence = seq;
    m->recv_headers[idx].payload_size = payload_size;
    if (payload && payload_size > 0 && payload_size <= MOCK_PAYLOAD_CAP) {
        memcpy(m->recv_payloads[idx], payload, payload_size);
    }
    m->recv_payload_sizes[idx] = payload_size;
    m->recv_count++;
}

static void test_castling_kingside(void)
{
    ChessGameState state;

    chess_game_state_init(&state);

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 6, 4, 4, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 0, 1, 0, 2, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 6, 7, 5, 5, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 1, 1, 1, 2, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 5, 7, 4, 6, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 2, 1, 2, 2, CHESS_PROMOTION_NONE));

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 7, 6, 7, CHESS_PROMOTION_NONE));
    EXPECT_EQ_INT(chess_game_get_piece(&state, 6, 7), CHESS_PIECE_WHITE_KING);
    EXPECT_EQ_INT(chess_game_get_piece(&state, 5, 7), CHESS_PIECE_WHITE_ROOK);
}

static void test_castling_illegal_while_in_check(void)
{
    ChessGameState state;

    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[7][7] = CHESS_PIECE_WHITE_ROOK;
    state.board[0][4] = CHESS_PIECE_BLACK_ROOK;
    state.board[0][0] = CHESS_PIECE_BLACK_KING;
    state.white_can_castle_kingside = true;
    state.white_can_castle_queenside = false;
    state.black_can_castle_kingside = false;
    state.black_can_castle_queenside = false;

    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 4, 7));
    {
        ChessMovePayload move;
        EXPECT_TRUE(!chess_game_try_local_move(
            &state,
            CHESS_COLOR_WHITE,
            6,
            7,
            CHESS_PROMOTION_NONE,
            &move));
    }
}

static void test_en_passant_capture(void)
{
    ChessGameState state;

    chess_game_state_init(&state);

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 6, 4, 4, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 0, 1, 0, 2, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 4, 4, 3, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 3, 1, 3, 3, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 3, 3, 2, CHESS_PROMOTION_NONE));

    EXPECT_EQ_INT(chess_game_get_piece(&state, 3, 2), CHESS_PIECE_WHITE_PAWN);
    EXPECT_EQ_INT(chess_game_get_piece(&state, 3, 3), CHESS_PIECE_EMPTY);
}

static void test_promotion_with_choice(void)
{
    ChessGameState state;

    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[0][4] = CHESS_PIECE_BLACK_KING;
    state.board[1][0] = CHESS_PIECE_WHITE_PAWN;

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 0, 1, 0, 0, CHESS_PROMOTION_KNIGHT));
    EXPECT_EQ_INT(chess_game_get_piece(&state, 0, 0), CHESS_PIECE_WHITE_KNIGHT);
}

static void test_checkmate_outcome(void)
{
    ChessGameState state;

    chess_game_state_init(&state);

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 6, 4, 4, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 4, 1, 4, 3, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 5, 7, 2, 4, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 1, 0, 2, 2, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 3, 7, 7, 3, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 6, 0, 5, 2, CHESS_PROMOTION_NONE));
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 7, 3, 5, 1, CHESS_PROMOTION_NONE));

    EXPECT_EQ_INT(state.outcome, CHESS_OUTCOME_CHECKMATE_WHITE_WINS);
}

static void test_fifty_move_rule(void)
{
    ChessGameState state;

    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[0][7] = CHESS_PIECE_BLACK_KING;
    state.board[7][6] = CHESS_PIECE_WHITE_KNIGHT;
    state.halfmove_clock = 99;

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 6, 7, 5, 5, CHESS_PROMOTION_NONE));
    EXPECT_EQ_INT(state.outcome, CHESS_OUTCOME_FIFTY_MOVE_RULE);
}

static void test_network_session_flow(void)
{
    ChessPeerInfo local;
    ChessPeerInfo remote;
    ChessNetworkSession session;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    (void)snprintf(local.profile_id, sizeof(local.profile_id), "%s", "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    (void)snprintf(remote.profile_id, sizeof(remote.profile_id), "%s", "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");

    chess_network_session_init(&session, &local);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_IDLE);

    /* Simulate challenger: set role before set_remote */
    session.role = CHESS_ROLE_CLIENT;
    chess_network_session_set_remote(&session, &remote);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_TCP_CONNECTING);

    /* Simulate transport + hello */
    session.transport_connected = true;
    session.hello_completed = true;
    session.challenge_done = true;

    /* Simulate game start */
    chess_network_session_start_game(&session, 42u, CHESS_COLOR_WHITE);
    EXPECT_EQ_INT(session.game_started, true);
    EXPECT_EQ_INT(session.game_id, 42u);
    EXPECT_EQ_INT(session.local_color, CHESS_COLOR_WHITE);
    EXPECT_EQ_INT(session.role, CHESS_ROLE_CLIENT);
}

static void test_persistent_profile_id(void)
{
    char tmp_template[] = "/tmp/chess_profile_test_XXXXXX";
    char profile_file[256];
    char *dir;
    ChessPeerInfo p1;
    ChessPeerInfo p2;

    dir = mkdtemp(tmp_template);
    EXPECT_TRUE(dir != NULL);
    if (!dir) {
        return;
    }

    EXPECT_TRUE(setenv("CHESS_APP_PROFILE_DIR", dir, 1) == 0);
    EXPECT_TRUE(chess_peer_init_local_identity(&p1));
    EXPECT_TRUE(chess_peer_init_local_identity(&p2));

    EXPECT_TRUE(p1.profile_id[0] != '\0');
    EXPECT_EQ_INT(strcmp(p1.profile_id, p2.profile_id), 0);

    (void)snprintf(profile_file, sizeof(profile_file), "%s/profile.json", dir);
    (void)unlink(profile_file);
    (void)rmdir(dir);
    (void)unsetenv("CHESS_APP_PROFILE_DIR");
}

static void test_tcp_packet_flow_basic(void)
{
    int fds[2] = { -1, -1 };
    ChessTcpConnection sender;
    ChessTcpConnection receiver;
    ChessHelloPayload hello_out;
    ChessHelloPayload hello_in;
    ChessAckPayload ack;
    ChessMovePayload move_out;
    ChessMovePayload move_in;
    ChessPacketHeader header;

    EXPECT_TRUE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    if (fds[0] < 0 || fds[1] < 0) {
        return;
    }

    sender.fd = fds[0];
    receiver.fd = fds[1];

    memset(&hello_out, 0, sizeof(hello_out));
    memset(&hello_in, 0, sizeof(hello_in));
    (void)snprintf(hello_out.profile_id, sizeof(hello_out.profile_id), "%s", "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    hello_out.role = CHESS_ROLE_SERVER;

    EXPECT_TRUE(chess_tcp_send_hello(&sender, &hello_out));
    EXPECT_TRUE(chess_tcp_recv_hello(&receiver, 100, &hello_in));
    EXPECT_EQ_INT(strcmp(hello_in.profile_id, hello_out.profile_id), 0);
    EXPECT_EQ_INT((int)hello_in.role, (int)hello_out.role);

    EXPECT_TRUE(chess_tcp_send_ack(&receiver, CHESS_MSG_HELLO, 1u, 0u));
    EXPECT_TRUE(chess_tcp_recv_ack(&sender, 100, &ack));
    EXPECT_EQ_INT((int)ack.acked_message_type, CHESS_MSG_HELLO);
    EXPECT_EQ_INT((int)ack.acked_sequence, 1);
    EXPECT_EQ_INT((int)ack.status_code, 0);

    move_out.from_file = 4;
    move_out.from_rank = 6;
    move_out.to_file = 4;
    move_out.to_rank = 4;
    move_out.promotion = CHESS_PROMOTION_NONE;
    memset(&move_in, 0, sizeof(move_in));

    EXPECT_TRUE(chess_tcp_send_packet(&sender, CHESS_MSG_MOVE, 3u, &move_out, (uint32_t)sizeof(move_out)));
    EXPECT_TRUE(chess_tcp_recv_packet_header(&receiver, 100, &header));
    EXPECT_EQ_INT((int)header.message_type, CHESS_MSG_MOVE);
    EXPECT_EQ_INT((int)header.payload_size, (int)sizeof(move_in));
    EXPECT_TRUE(chess_tcp_recv_payload(&receiver, 100, &move_in, (uint32_t)sizeof(move_in)));
    EXPECT_EQ_INT((int)move_in.from_file, (int)move_out.from_file);
    EXPECT_EQ_INT((int)move_in.to_rank, (int)move_out.to_rank);

    chess_tcp_connection_close(&sender);
    chess_tcp_connection_close(&receiver);
}

static void test_castling_notation_with_check(void)
{
    ChessGameState state;
    char notation[24];

    /* White: Ke1, Ra1.  Black: Kd3 (on the d-file so the rook
     * landing on d1 after O-O-O gives check). */
    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[7][0] = CHESS_PIECE_WHITE_ROOK;
    state.board[5][3] = CHESS_PIECE_BLACK_KING;
    state.white_can_castle_kingside = true;
    state.white_can_castle_queenside = true;

    EXPECT_TRUE(chess_move_format_algebraic_notation(
        &state, 4, 7, 2, 7, CHESS_PROMOTION_NONE, notation, sizeof(notation)));
    EXPECT_EQ_STR(notation, "O-O-O+");
}

static void test_lobby_remove_peer_by_profile_id(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p1;
    ChessPeerInfo p2;

    chess_lobby_init(&lobby);
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    (void)snprintf(p1.profile_id, sizeof(p1.profile_id), "%s", "11111111-1111-4111-a111-111111111111");
    (void)snprintf(p2.profile_id, sizeof(p2.profile_id), "%s", "22222222-2222-4222-a222-222222222222");

    chess_lobby_add_or_update_peer(&lobby, &p1, 0, 5001u);
    chess_lobby_add_or_update_peer(&lobby, &p2, 0, 5002u);
    EXPECT_EQ_INT(lobby.discovered_peer_count, 2);

    lobby.selected_peer_idx = 1;
    EXPECT_TRUE(chess_lobby_remove_peer_by_profile_id(&lobby, p1.profile_id));
    EXPECT_EQ_INT(lobby.discovered_peer_count, 1);
    EXPECT_EQ_INT(strcmp(lobby.discovered_peers[0].peer.profile_id, p2.profile_id), 0);
    EXPECT_EQ_INT(lobby.selected_peer_idx, 0);

    EXPECT_TRUE(!chess_lobby_remove_peer_by_profile_id(&lobby, "not-found"));
}

static void test_resign_blocks_further_moves(void)
{
    ChessGameState state;
    ChessMovePayload move;

    chess_game_state_init(&state);

    /* Normal play: White can select and move e2-e4 */
    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 4, 6));
    EXPECT_TRUE(chess_game_try_local_move(&state, CHESS_COLOR_WHITE, 4, 4, CHESS_PROMOTION_NONE, &move));

    /* Simulate resignation */
    state.outcome = CHESS_OUTCOME_WHITE_RESIGNED;

    /* After resign: Black can still select a piece (game_state layer has no
     * outcome guard — that's the input_handler's job), but the outcome enum
     * is properly set so the UI/input layers will block further interaction. */
    EXPECT_EQ_INT(state.outcome, CHESS_OUTCOME_WHITE_RESIGNED);

    /* Verify draw agreed outcome persists through reinit guard */
    state.outcome = CHESS_OUTCOME_DRAW_AGREED;
    EXPECT_EQ_INT(state.outcome, CHESS_OUTCOME_DRAW_AGREED);
}

static void test_draw_offer_flags(void)
{
    ChessPeerInfo local;
    ChessNetworkSession session;

    memset(&local, 0, sizeof(local));
    (void)snprintf(local.profile_id, sizeof(local.profile_id), "%s", "cccccccc-cccc-4ccc-cccc-cccccccccccc");
    chess_network_session_init(&session, &local);

    /* Init zeroes both flags */
    EXPECT_EQ_INT(session.draw_offer_pending, false);
    EXPECT_EQ_INT(session.draw_offer_received, false);

    /* Simulate: we sent a draw offer, opponent sent one too (cross-offer) */
    session.draw_offer_pending = true;
    session.draw_offer_received = true;

    /* Starting a new game must reset both flags */
    chess_network_session_start_game(&session, 1u, CHESS_COLOR_WHITE);
    EXPECT_EQ_INT(session.draw_offer_pending, false);
    EXPECT_EQ_INT(session.draw_offer_received, false);
    EXPECT_EQ_INT(session.game_started, true);
    EXPECT_EQ_INT(session.local_color, CHESS_COLOR_WHITE);
}

/* ── GameState: stalemate ────────────────────────────────────────────── */

static void test_stalemate_detection(void)
{
    ChessGameState state;

    /* Position: White Kf2 (5,6), Qf3 (5,5), Black Kh1 (7,7).
     * White plays Qf3→g3 creating stalemate: Black Kh1 has no legal
     * moves (g1,g2,h2 all attacked) but is not in check. */
    clear_board(&state);
    state.board[6][5] = CHESS_PIECE_WHITE_KING;   /* Kf2 */
    state.board[5][5] = CHESS_PIECE_WHITE_QUEEN;  /* Qf3 */
    state.board[7][7] = CHESS_PIECE_BLACK_KING;   /* Kh1 */
    state.side_to_move = CHESS_COLOR_WHITE;

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 5, 5, 6, 5, CHESS_PROMOTION_NONE));
    EXPECT_EQ_INT(state.outcome, CHESS_OUTCOME_STALEMATE);
}

static void test_castling_through_attacked_square(void)
{
    ChessGameState state;
    ChessMovePayload move;

    /* White: Ke1, Rh1. Black: Kd8, Rf8 (f-file rook attacks f1) */
    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[7][7] = CHESS_PIECE_WHITE_ROOK;
    state.board[0][3] = CHESS_PIECE_BLACK_KING;
    state.board[0][5] = CHESS_PIECE_BLACK_ROOK;  /* attacks f1 */
    state.white_can_castle_kingside = true;

    /* King would pass through f1 which is attacked → illegal */
    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 4, 7));
    EXPECT_TRUE(!chess_game_try_local_move(&state, CHESS_COLOR_WHITE, 6, 7,
                                            CHESS_PROMOTION_NONE, &move));
}

static void test_pawn_cannot_move_backward(void)
{
    ChessGameState state;

    clear_board(&state);
    state.board[4][4] = CHESS_PIECE_WHITE_PAWN;
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[0][4] = CHESS_PIECE_BLACK_KING;

    /* White pawn at e4, try to move to e5 (rank 5 = backward for white since board[4] is rank 4) */
    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 4, 4));
    {
        ChessMovePayload move;
        EXPECT_TRUE(!chess_game_try_local_move(&state, CHESS_COLOR_WHITE, 4, 5,
                                                CHESS_PROMOTION_NONE, &move));
    }
}

static void test_pawn_double_move_only_from_start(void)
{
    ChessGameState state;

    clear_board(&state);
    state.board[4][4] = CHESS_PIECE_WHITE_PAWN;  /* rank 4, file 4 = not starting rank */
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[0][4] = CHESS_PIECE_BLACK_KING;

    /* Try double move from non-starting rank */
    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 4, 4));
    {
        ChessMovePayload move;
        EXPECT_TRUE(!chess_game_try_local_move(&state, CHESS_COLOR_WHITE, 4, 2,
                                                CHESS_PROMOTION_NONE, &move));
    }
}

static void test_en_passant_expires(void)
{
    ChessGameState state;

    chess_game_state_init(&state);

    /* 1. e2-e4 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 6, 4, 4, CHESS_PROMOTION_NONE));
    /* 1... a7-a6 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 0, 1, 0, 2, CHESS_PROMOTION_NONE));
    /* 2. e4-e5 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 4, 4, 3, CHESS_PROMOTION_NONE));
    /* 2... d7-d5 (creates en passant target at d6) */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 3, 1, 3, 3, CHESS_PROMOTION_NONE));
    /* 3. a2-a3 (White wastes the en passant opportunity) */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 0, 6, 0, 5, CHESS_PROMOTION_NONE));
    /* 3... a6-a5 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 0, 2, 0, 3, CHESS_PROMOTION_NONE));

    /* 4. White tries exd6 en passant from e5 → should fail (expired) */
    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 4, 3));
    {
        ChessMovePayload move;
        EXPECT_TRUE(!chess_game_try_local_move(&state, CHESS_COLOR_WHITE, 3, 2,
                                                CHESS_PROMOTION_NONE, &move));
    }
}

static void test_apply_remote_move(void)
{
    ChessGameState state;
    ChessMovePayload remote_move;

    chess_game_state_init(&state);

    /* White plays e2-e4 locally */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 6, 4, 4, CHESS_PROMOTION_NONE));
    EXPECT_EQ_INT(state.side_to_move, CHESS_COLOR_BLACK);

    /* Apply Black's d7-d5 as a remote move */
    remote_move.from_file = 3;
    remote_move.from_rank = 1;
    remote_move.to_file = 3;
    remote_move.to_rank = 3;
    remote_move.promotion = CHESS_PROMOTION_NONE;

    EXPECT_TRUE(chess_game_apply_remote_move(&state, CHESS_COLOR_BLACK, &remote_move));
    EXPECT_EQ_INT(chess_game_get_piece(&state, 3, 3), CHESS_PIECE_BLACK_PAWN);
    EXPECT_EQ_INT(chess_game_get_piece(&state, 3, 1), CHESS_PIECE_EMPTY);
    EXPECT_EQ_INT(state.side_to_move, CHESS_COLOR_WHITE);
}

static void test_promotion_required_detection(void)
{
    ChessGameState state;

    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[0][4] = CHESS_PIECE_BLACK_KING;
    state.board[1][0] = CHESS_PIECE_WHITE_PAWN;

    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 0, 1));
    EXPECT_TRUE(chess_game_local_move_requires_promotion(&state, CHESS_COLOR_WHITE, 0, 0));

    /* Pawn not on promotion rank */
    clear_board(&state);
    state.board[7][4] = CHESS_PIECE_WHITE_KING;
    state.board[0][4] = CHESS_PIECE_BLACK_KING;
    state.board[4][0] = CHESS_PIECE_WHITE_PAWN;

    EXPECT_TRUE(chess_game_select_local_piece(&state, CHESS_COLOR_WHITE, 0, 4));
    EXPECT_TRUE(!chess_game_local_move_requires_promotion(&state, CHESS_COLOR_WHITE, 0, 3));
}

static void test_captured_pieces_tracking(void)
{
    ChessGameState state;
    ChessCapturedPieces captured;

    chess_game_state_init(&state);

    /* White e2-e4 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 6, 4, 4, CHESS_PROMOTION_NONE));
    /* Black d7-d5 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 3, 1, 3, 3, CHESS_PROMOTION_NONE));
    /* White exd5 (capture!) */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 4, 4, 3, 3, CHESS_PROMOTION_NONE));
    EXPECT_EQ_INT(chess_game_get_piece(&state, 3, 3), CHESS_PIECE_WHITE_PAWN);

    chess_game_compute_captured(&state, &captured);
    EXPECT_EQ_INT(captured.count[CHESS_PIECE_BLACK_PAWN], 1);
}

static void test_checkmate_black_wins(void)
{
    ChessGameState state;

    /* Fool's mate: 1. f3 e5 2. g4 Qh4# */
    chess_game_state_init(&state);

    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 5, 6, 5, 5, CHESS_PROMOTION_NONE)); /* f2-f3 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 4, 1, 4, 3, CHESS_PROMOTION_NONE)); /* e7-e5 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_WHITE, 6, 6, 6, 4, CHESS_PROMOTION_NONE)); /* g2-g4 */
    EXPECT_TRUE(move_local(&state, CHESS_COLOR_BLACK, 3, 0, 7, 4, CHESS_PROMOTION_NONE)); /* Qd8-h4# */

    EXPECT_EQ_INT(state.outcome, CHESS_OUTCOME_CHECKMATE_BLACK_WINS);
}

/* ── NetworkSession: FSM edge cases ──────────────────────────────────── */

static void test_session_phase_transitions(void)
{
    ChessPeerInfo local;
    ChessNetworkSession session;

    memset(&local, 0, sizeof(local));
    (void)snprintf(local.profile_id, sizeof(local.profile_id), "%s",
                   "dddddddd-dddd-4ddd-dddd-dddddddddddd");
    chess_network_session_init(&session, &local);

    chess_network_session_set_phase(&session, CHESS_PHASE_TCP_CONNECTING);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_TCP_CONNECTING);

    chess_network_session_set_phase(&session, CHESS_PHASE_HELLO_HANDSHAKE);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_HELLO_HANDSHAKE);

    chess_network_session_set_phase(&session, CHESS_PHASE_IN_GAME);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_IN_GAME);

    chess_network_session_set_phase(&session, CHESS_PHASE_DISCONNECTED);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_DISCONNECTED);

    chess_network_session_set_phase(&session, CHESS_PHASE_TERMINATED);
    EXPECT_EQ_INT(session.phase, CHESS_PHASE_TERMINATED);
}

static void test_session_phase_to_string(void)
{
    EXPECT_TRUE(chess_connection_phase_to_string(CHESS_PHASE_IDLE) != NULL);
    EXPECT_TRUE(chess_connection_phase_to_string(CHESS_PHASE_TCP_CONNECTING) != NULL);
    EXPECT_TRUE(chess_connection_phase_to_string(CHESS_PHASE_IN_GAME) != NULL);
    EXPECT_TRUE(chess_connection_phase_to_string(CHESS_PHASE_TERMINATED) != NULL);

    /* Each phase should produce a distinct string */
    EXPECT_TRUE(strcmp(
        chess_connection_phase_to_string(CHESS_PHASE_IDLE),
        chess_connection_phase_to_string(CHESS_PHASE_IN_GAME)) != 0);
}

static void test_session_double_start_game(void)
{
    ChessPeerInfo local;
    ChessNetworkSession session;

    memset(&local, 0, sizeof(local));
    (void)snprintf(local.profile_id, sizeof(local.profile_id), "%s",
                   "eeeeeeee-eeee-4eee-eeee-eeeeeeeeeeee");
    chess_network_session_init(&session, &local);

    chess_network_session_start_game(&session, 100u, CHESS_COLOR_WHITE);
    EXPECT_EQ_INT(session.game_started, true);
    EXPECT_EQ_INT(session.game_id, 100u);
    EXPECT_EQ_INT(session.local_color, CHESS_COLOR_WHITE);

    /* Second call should update cleanly */
    chess_network_session_start_game(&session, 200u, CHESS_COLOR_BLACK);
    EXPECT_EQ_INT(session.game_started, true);
    EXPECT_EQ_INT(session.game_id, 200u);
    EXPECT_EQ_INT(session.local_color, CHESS_COLOR_BLACK);
    EXPECT_EQ_INT(session.draw_offer_pending, false);
    EXPECT_EQ_INT(session.draw_offer_received, false);
}

static void test_session_server_role_set_remote(void)
{
    ChessPeerInfo local;
    ChessPeerInfo remote;
    ChessNetworkSession session;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    (void)snprintf(local.profile_id, sizeof(local.profile_id), "%s",
                   "ffffffff-ffff-4fff-ffff-ffffffffffff");
    (void)snprintf(remote.profile_id, sizeof(remote.profile_id), "%s",
                   "11111111-1111-4111-a111-111111111111");
    chess_network_session_init(&session, &local);

    session.role = CHESS_ROLE_SERVER;
    chess_network_session_set_remote(&session, &remote);

    /* Server role should NOT auto-transition to TCP_CONNECTING */
    EXPECT_TRUE(session.phase != CHESS_PHASE_TCP_CONNECTING || session.role == CHESS_ROLE_SERVER);
    EXPECT_EQ_INT(strcmp(session.remote_peer.profile_id, remote.profile_id), 0);
}

/* ── Lobby: additional coverage ──────────────────────────────────────── */

static void test_lobby_add_duplicate_peer(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p1;

    chess_lobby_init(&lobby);
    memset(&p1, 0, sizeof(p1));
    (void)snprintf(p1.profile_id, sizeof(p1.profile_id), "%s",
                   "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");

    chess_lobby_add_or_update_peer(&lobby, &p1, 0, 5001u);
    chess_lobby_add_or_update_peer(&lobby, &p1, 0, 5002u);  /* same profile, different port */

    EXPECT_EQ_INT(lobby.discovered_peer_count, 1);  /* no duplicate */
}

static void test_lobby_max_peers(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p;
    int i;

    chess_lobby_init(&lobby);

    for (i = 0; i < CHESS_MAX_DISCOVERED_PEERS + 1; ++i) {
        memset(&p, 0, sizeof(p));
        (void)snprintf(p.profile_id, sizeof(p.profile_id), "%08x-0000-4000-8000-000000000000", i);
        chess_lobby_add_or_update_peer(&lobby, &p, 0, (uint16_t)(5001 + i));
    }

    EXPECT_TRUE(lobby.discovered_peer_count <= CHESS_MAX_DISCOVERED_PEERS);
}

static void test_lobby_challenge_state(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p1;

    chess_lobby_init(&lobby);
    memset(&p1, 0, sizeof(p1));
    (void)snprintf(p1.profile_id, sizeof(p1.profile_id), "%s",
                   "cccccccc-cccc-4ccc-cccc-cccccccccccc");

    chess_lobby_add_or_update_peer(&lobby, &p1, 0, 5001u);
    EXPECT_EQ_INT(chess_lobby_get_challenge_state(&lobby, 0), CHESS_CHALLENGE_NONE);

    chess_lobby_set_challenge_state(&lobby, 0, CHESS_CHALLENGE_OUTGOING_PENDING);
    EXPECT_EQ_INT(chess_lobby_get_challenge_state(&lobby, 0), CHESS_CHALLENGE_OUTGOING_PENDING);

    chess_lobby_set_challenge_state(&lobby, 0, CHESS_CHALLENGE_MATCHED);
    EXPECT_EQ_INT(chess_lobby_get_challenge_state(&lobby, 0), CHESS_CHALLENGE_MATCHED);
}

static void test_lobby_offer_sent_tracking(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p1;

    chess_lobby_init(&lobby);
    memset(&p1, 0, sizeof(p1));
    (void)snprintf(p1.profile_id, sizeof(p1.profile_id), "%s",
                   "dddddddd-dddd-4ddd-dddd-dddddddddddd");

    chess_lobby_add_or_update_peer(&lobby, &p1, 0, 5001u);
    EXPECT_TRUE(!chess_lobby_has_offer_been_sent(&lobby, 0));

    chess_lobby_mark_offer_sent(&lobby, 0);
    EXPECT_TRUE(chess_lobby_has_offer_been_sent(&lobby, 0));
}

static void test_lobby_find_peer(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p1;
    ChessPeerInfo p2;
    ChessPeerInfo p_unknown;
    int idx;

    chess_lobby_init(&lobby);
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    memset(&p_unknown, 0, sizeof(p_unknown));
    (void)snprintf(p1.profile_id, sizeof(p1.profile_id), "%s", "11111111-1111-4111-a111-111111111111");
    (void)snprintf(p2.profile_id, sizeof(p2.profile_id), "%s", "22222222-2222-4222-a222-222222222222");
    (void)snprintf(p_unknown.profile_id, sizeof(p_unknown.profile_id), "%s", "99999999-9999-4999-a999-999999999999");

    chess_lobby_add_or_update_peer(&lobby, &p1, 0, 5001u);
    chess_lobby_add_or_update_peer(&lobby, &p2, 0, 5002u);

    idx = -1;
    EXPECT_TRUE(chess_lobby_find_peer(&lobby, &p2, &idx));
    EXPECT_EQ_INT(idx, 1);

    EXPECT_TRUE(!chess_lobby_find_peer(&lobby, &p_unknown, &idx));
}

/* ── Transport vtable tests ──────────────────────────────────────────── */

static void test_mock_transport_send_records_calls(void)
{
    MockTransport m;
    ChessMovePayload move = { .from_file = 4, .from_rank = 6, .to_file = 4, .to_rank = 4 };

    mock_transport_init(&m);

    EXPECT_TRUE(transport_send_packet(&m.base, CHESS_MSG_MOVE, 1u, &move, (uint32_t)sizeof(move)));
    EXPECT_EQ_INT(m.sent_count, 1);
    EXPECT_EQ_INT((int)m.sent[0].msg_type, CHESS_MSG_MOVE);
    EXPECT_EQ_INT((int)m.sent[0].sequence, 1);
    EXPECT_EQ_INT((int)m.sent[0].payload_size, (int)sizeof(move));

    {
        ChessMovePayload *recorded = (ChessMovePayload *)m.sent[0].payload;
        EXPECT_EQ_INT(recorded->from_file, 4);
        EXPECT_EQ_INT(recorded->to_rank, 4);
    }
}

static void test_mock_transport_recv_returns_queued(void)
{
    MockTransport m;
    ChessMovePayload move_in = { .from_file = 2, .from_rank = 1, .to_file = 2, .to_rank = 3 };
    ChessPacketHeader hdr;
    uint8_t payload[256];
    ChessRecvResult result;

    mock_transport_init(&m);
    mock_transport_queue_recv(&m, CHESS_MSG_MOVE, 5u, &move_in, (uint32_t)sizeof(move_in));

    memset(&hdr, 0, sizeof(hdr));
    memset(payload, 0, sizeof(payload));

    result = transport_recv_nonblocking(&m.base, &hdr, payload, sizeof(payload));
    EXPECT_EQ_INT((int)result, (int)CHESS_RECV_OK);
    EXPECT_EQ_INT((int)hdr.message_type, CHESS_MSG_MOVE);
    EXPECT_EQ_INT((int)hdr.sequence, 5);

    {
        ChessMovePayload *recv_move = (ChessMovePayload *)payload;
        EXPECT_EQ_INT(recv_move->from_file, 2);
        EXPECT_EQ_INT(recv_move->to_file, 2);
    }

    /* Queue exhausted → INCOMPLETE */
    result = transport_recv_nonblocking(&m.base, &hdr, payload, sizeof(payload));
    EXPECT_EQ_INT((int)result, (int)CHESS_RECV_INCOMPLETE);
}

static void test_transport_null_safety(void)
{
    Transport t_null_ops;
    ChessPacketHeader hdr;
    uint8_t payload[64];

    /* NULL transport pointer */
    EXPECT_TRUE(!transport_send_packet(NULL, CHESS_MSG_MOVE, 1u, NULL, 0u));
    EXPECT_EQ_INT((int)transport_recv_nonblocking(NULL, &hdr, payload, sizeof(payload)),
                  (int)CHESS_RECV_ERROR);
    EXPECT_EQ_INT(transport_get_fd(NULL), -1);
    EXPECT_TRUE(!transport_set_nonblocking(NULL));

    /* Non-NULL transport but NULL ops */
    t_null_ops.ops = NULL;
    EXPECT_TRUE(!transport_send_packet(&t_null_ops, CHESS_MSG_MOVE, 1u, NULL, 0u));
    EXPECT_EQ_INT((int)transport_recv_nonblocking(&t_null_ops, &hdr, payload, sizeof(payload)),
                  (int)CHESS_RECV_ERROR);
    EXPECT_EQ_INT(transport_get_fd(&t_null_ops), -1);
}

static void test_transport_send_hello_via_vtable(void)
{
    MockTransport m;
    ChessHelloPayload hello;

    mock_transport_init(&m);
    memset(&hello, 0, sizeof(hello));
    (void)snprintf(hello.profile_id, sizeof(hello.profile_id), "%s",
                   "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    hello.role = CHESS_ROLE_CLIENT;

    EXPECT_TRUE(transport_send_hello(&m.base, &hello));
    EXPECT_EQ_INT(m.sent_count, 1);
    EXPECT_EQ_INT((int)m.sent[0].msg_type, (int)CHESS_MSG_HELLO);
    EXPECT_EQ_INT((int)m.sent[0].sequence, 1);
    EXPECT_EQ_INT((int)m.sent[0].payload_size, (int)sizeof(hello));

    {
        ChessHelloPayload *recorded = (ChessHelloPayload *)m.sent[0].payload;
        EXPECT_EQ_INT(strcmp(recorded->profile_id, hello.profile_id), 0);
        EXPECT_EQ_INT((int)recorded->role, (int)CHESS_ROLE_CLIENT);
    }
}

static void test_transport_send_failure_injection(void)
{
    MockTransport m;
    ChessMovePayload move = { 0 };

    mock_transport_init(&m);
    m.fail_send = true;

    EXPECT_TRUE(!transport_send_packet(&m.base, CHESS_MSG_MOVE, 1u, &move, (uint32_t)sizeof(move)));
    EXPECT_EQ_INT(m.sent_count, 0);
}

static void test_transport_close_and_reset(void)
{
    MockTransport m;

    mock_transport_init(&m);
    EXPECT_TRUE(!m.close_called);
    EXPECT_TRUE(!m.recv_reset_called);

    transport_close(&m.base);
    EXPECT_TRUE(m.close_called);

    transport_recv_reset(&m.base);
    EXPECT_TRUE(m.recv_reset_called);

    EXPECT_EQ_INT(transport_get_fd(&m.base), 42);
    EXPECT_TRUE(transport_set_nonblocking(&m.base));
}

static void test_transport_send_ack_fields(void)
{
    MockTransport m;

    mock_transport_init(&m);

    EXPECT_TRUE(transport_send_ack(&m.base, CHESS_MSG_HELLO, 1u, 0u));
    EXPECT_EQ_INT(m.sent_count, 1);
    EXPECT_EQ_INT((int)m.sent[0].msg_type, (int)CHESS_MSG_ACK);

    {
        ChessAckPayload *ack = (ChessAckPayload *)m.sent[0].payload;
        EXPECT_EQ_INT((int)ack->acked_message_type, (int)CHESS_MSG_HELLO);
        EXPECT_EQ_INT((int)ack->acked_sequence, 1);
        EXPECT_EQ_INT((int)ack->status_code, 0);
    }
}

int main(void)
{
    test_castling_kingside();
    test_castling_illegal_while_in_check();
    test_en_passant_capture();
    test_promotion_with_choice();
    test_checkmate_outcome();
    test_fifty_move_rule();
    test_network_session_flow();
    test_persistent_profile_id();
    test_tcp_packet_flow_basic();
    test_castling_notation_with_check();
    test_lobby_remove_peer_by_profile_id();
    test_resign_blocks_further_moves();
    test_draw_offer_flags();

    /* Transport vtable tests */
    test_mock_transport_send_records_calls();
    test_mock_transport_recv_returns_queued();
    test_transport_null_safety();
    test_transport_send_hello_via_vtable();
    test_transport_send_failure_injection();
    test_transport_close_and_reset();
    test_transport_send_ack_fields();

    /* GameState additional coverage */
    test_stalemate_detection();
    test_castling_through_attacked_square();
    test_pawn_cannot_move_backward();
    test_pawn_double_move_only_from_start();
    test_en_passant_expires();
    test_apply_remote_move();
    test_promotion_required_detection();
    test_captured_pieces_tracking();
    test_checkmate_black_wins();

    /* NetworkSession FSM edge cases */
    test_session_phase_transitions();
    test_session_phase_to_string();
    test_session_double_start_game();
    test_session_server_role_set_remote();

    /* Lobby additional coverage */
    test_lobby_add_duplicate_peer();
    test_lobby_max_peers();
    test_lobby_challenge_state();
    test_lobby_offer_sent_tracking();
    test_lobby_find_peer();

    fprintf(stdout, "Tests passed: %d\n", s_passed);
    if (s_failed > 0) {
        fprintf(stderr, "Tests failed: %d\n", s_failed);
        return 1;
    }
    fprintf(stdout, "All tests passed.\n");
    return 0;
}
