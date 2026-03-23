#include "chess_app/game_state.h"
#include "chess_app/lobby_state.h"
#include "chess_app/network_session.h"
#include "chess_app/network_tcp.h"

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
    local.ipv4_host_order = 10u;
    remote.ipv4_host_order = 20u;
    (void)snprintf(local.uuid, sizeof(local.uuid), "%s", "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    (void)snprintf(remote.uuid, sizeof(remote.uuid), "%s", "bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");

    chess_network_session_init(&session, &local);
    EXPECT_EQ_INT(session.state, CHESS_NET_IDLE_DISCOVERY);

    chess_network_session_set_remote(&session, &remote);
    EXPECT_EQ_INT(session.state, CHESS_NET_PEER_FOUND);

    chess_network_session_step(&session);
    EXPECT_EQ_INT(session.state, CHESS_NET_ELECTION);

    chess_network_session_step(&session);
    EXPECT_EQ_INT(session.state, CHESS_NET_CONNECTING);
    EXPECT_EQ_INT(session.role, CHESS_ROLE_SERVER);

    chess_network_session_set_transport_ready(&session, true);
    chess_network_session_step(&session);
    EXPECT_EQ_INT(session.state, CHESS_NET_IN_GAME);
}

static void test_role_election_uuid_fallback(void)
{
    ChessPeerInfo local;
    ChessPeerInfo remote;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    local.ipv4_host_order = 0x7f000001u;
    remote.ipv4_host_order = 0x7f000001u;
    (void)snprintf(local.uuid, sizeof(local.uuid), "%s", "11111111-1111-4111-a111-111111111111");
    (void)snprintf(remote.uuid, sizeof(remote.uuid), "%s", "22222222-2222-4222-a222-222222222222");

    EXPECT_EQ_INT(chess_elect_role(&local, &remote), CHESS_ROLE_SERVER);
    EXPECT_EQ_INT(chess_elect_role(&remote, &local), CHESS_ROLE_CLIENT);
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
    EXPECT_TRUE(p1.uuid[0] != '\0');
    EXPECT_TRUE(p2.uuid[0] != '\0');

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
    (void)snprintf(hello_out.uuid, sizeof(hello_out.uuid), "%s", "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    hello_out.role = CHESS_ROLE_SERVER;

    EXPECT_TRUE(chess_tcp_send_hello(&sender, &hello_out));
    EXPECT_TRUE(chess_tcp_recv_hello(&receiver, 100, &hello_in));
    EXPECT_EQ_INT(strcmp(hello_in.uuid, hello_out.uuid), 0);
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

static void test_lobby_remove_peer_by_uuid(void)
{
    ChessLobbyState lobby;
    ChessPeerInfo p1;
    ChessPeerInfo p2;

    chess_lobby_init(&lobby);
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    (void)snprintf(p1.uuid, sizeof(p1.uuid), "%s", "11111111-1111-4111-a111-111111111111");
    (void)snprintf(p2.uuid, sizeof(p2.uuid), "%s", "22222222-2222-4222-a222-222222222222");

    chess_lobby_add_or_update_peer(&lobby, &p1, 5001u);
    chess_lobby_add_or_update_peer(&lobby, &p2, 5002u);
    EXPECT_EQ_INT(lobby.discovered_peer_count, 2);

    lobby.selected_peer_idx = 1;
    EXPECT_TRUE(chess_lobby_remove_peer_by_uuid(&lobby, p1.uuid));
    EXPECT_EQ_INT(lobby.discovered_peer_count, 1);
    EXPECT_EQ_INT(strcmp(lobby.discovered_peers[0].peer.uuid, p2.uuid), 0);
    EXPECT_EQ_INT(lobby.selected_peer_idx, 0);

    EXPECT_TRUE(!chess_lobby_remove_peer_by_uuid(&lobby, "not-found"));
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
    test_role_election_uuid_fallback();
    test_persistent_profile_id();
    test_tcp_packet_flow_basic();
    test_lobby_remove_peer_by_uuid();

    fprintf(stdout, "Tests passed: %d\n", s_passed);
    if (s_failed > 0) {
        fprintf(stderr, "Tests failed: %d\n", s_failed);
        return 1;
    }
    fprintf(stdout, "All tests passed.\n");
    return 0;
}
