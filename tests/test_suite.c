#include "chess_app/game_state.h"
#include "chess_app/network_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    fprintf(stdout, "Tests passed: %d\n", s_passed);
    if (s_failed > 0) {
        fprintf(stderr, "Tests failed: %d\n", s_failed);
        return 1;
    }
    fprintf(stdout, "All tests passed.\n");
    return 0;
}
