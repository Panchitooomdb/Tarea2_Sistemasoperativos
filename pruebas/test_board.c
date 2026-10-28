#define _POSIX_C_SOURCE 200809L
#include "board.h"

#include <pthread.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FALLO linea %d: %s\n", __LINE__, #cond); failures++; } } while (0)

#define N(r, c) board_node_from_rc((r), (c))

static void test_graph(void) {
    printf("[graph] adyacencias y saltos\n");
    int edges = 0;
    for (int a = 0; a < NUM_NODES; a++)
        for (int b = 0; b < NUM_NODES; b++) {
            CHECK(board_adjacent(a, b) == board_adjacent(b, a));
            if (a < b && board_adjacent(a, b)) edges++;
        }
    CHECK(edges == 48); /* 40 de la cuadricula + 8 del trono */

    CHECK(!board_adjacent(N(0, 0), N(1, 1)));       /* sin diagonales en el 5x5 */
    CHECK(!board_adjacent(N(2, 4), NODE_TOP));      /* caso 8 del txt */
    CHECK(board_adjacent(NODE_LEFT, N(0, 0)));      /* diagonal del trono */
    CHECK(board_adjacent(NODE_RIGHT, N(0, 4)));
    CHECK(board_adjacent(NODE_TOP, N(0, 2)));
    CHECK(board_adjacent(NODE_LEFT, NODE_RIGHT));
    CHECK(board_adjacent(NODE_TOP, NODE_LEFT) && board_adjacent(NODE_TOP, NODE_RIGHT));
    CHECK(!board_adjacent(NODE_TOP, N(0, 1)));

    CHECK(board_jump_over(N(2, 2), N(2, 4)) == N(2, 3));
    CHECK(board_jump_over(N(0, 0), N(2, 0)) == N(1, 0));
    CHECK(board_jump_over(NODE_TOP, N(0, 0)) == NODE_LEFT);
    CHECK(board_jump_over(N(0, 0), NODE_TOP) == NODE_LEFT);
    CHECK(board_jump_over(N(1, 2), NODE_TOP) == N(0, 2));
    CHECK(board_jump_over(NODE_LEFT, N(1, 1)) == N(0, 1));
    CHECK(board_jump_over(NODE_LEFT, NODE_RIGHT) == -1);
    CHECK(board_jump_over(N(0, 0), N(2, 2)) == -1);
}

static void test_moves(void) {
    printf("[moves] validaciones y captura\n");
    Board b;
    int cap;
    board_init(&b);
    CHECK(board_place_puma(&b, N(2, 2)) == MOVE_OK);
    CHECK(board_place_puma(&b, N(3, 3)) == MOVE_ERR_ALREADY_PLACED);
    CHECK(board_place_dog(&b, 0, N(2, 3)) == MOVE_OK);
    CHECK(board_place_dog(&b, 1, N(2, 3)) == MOVE_ERR_OCCUPIED);
    CHECK(board_place_dog(&b, 99, N(0, 0)) == MOVE_ERR_BAD_PIECE);
    CHECK(board_move_puma(&b, N(2, 2), N(4, 4), &cap) == MOVE_ERR_NOT_ADJACENT);   /* caso 7 */
    CHECK(board_move_puma(&b, N(2, 2), N(2, 1), &cap) == MOVE_OK);
    CHECK(board_move_puma(&b, N(2, 2), N(2, 1), &cap) == MOVE_ERR_NOT_AT_ORIGIN);
    CHECK(board_move_puma(&b, N(2, 1), N(2, 3), &cap) == MOVE_ERR_NO_DOG_TO_JUMP);   /* (2,2) esta vacia */
    CHECK(board_move_puma(&b, N(2, 1), N(2, 2), &cap) == MOVE_OK);
    CHECK(board_move_dog(&b, 0, N(2, 3), N(2, 2)) == MOVE_ERR_OCCUPIED);           /* caso 1 */
    CHECK(board_move_puma(&b, N(2, 2), N(2, 4), &cap) == MOVE_CAPTURE && cap == 0);
    CHECK(board_captures(&b) == 1);
    CHECK(board_move_puma(&b, N(2, 4), N(2, 2), &cap) == MOVE_ERR_NO_DOG_TO_JUMP);
    CHECK(board_move_dog(&b, 0, N(2, 3), N(2, 2)) == MOVE_ERR_NOT_AT_ORIGIN);      /* ya capturado */
    CHECK(board_consistent(&b));
    board_destroy(&b);
}

static void test_win_conditions(void) {
    printf("[win] victoria de perros y del puma\n");
    Board b;
    int cap;

    /* Puma acorralado en (0,0): vecinos (0,1), (1,0), LEFT, y saltos bloqueados */
    board_init(&b);
    board_place_puma(&b, N(0, 0));
    int blockers[6] = {N(0, 1), N(0, 2), N(1, 0), N(2, 0), NODE_LEFT, NODE_TOP};
    for (int i = 0; i < 6; i++) board_place_dog(&b, i, blockers[i]);
    CHECK(board_status(&b) == GAME_DOGS_WIN);
    CHECK(board_move_dog(&b, 0, N(0, 1), N(1, 1)) == MOVE_ERR_GAME_OVER);
    board_destroy(&b);

    /* Puma captura 6 perros recorriendo el borde del tablero */
    board_init(&b);
    board_place_puma(&b, N(0, 0));
    int dogs[6] = {N(1, 0), N(3, 0), N(4, 1), N(4, 3), N(3, 4), N(1, 4)};
    for (int i = 0; i < 6; i++) board_place_dog(&b, i, dogs[i]);
    int path[7] = {N(0, 0), N(2, 0), N(4, 0), N(4, 2), N(4, 4), N(2, 4), N(0, 4)};
    for (int i = 0; i < 6; i++) CHECK(board_move_puma(&b, path[i], path[i + 1], &cap) == MOVE_CAPTURE);
    CHECK(board_status(&b) == GAME_PUMA_WINS);
    CHECK(board_move_puma(&b, N(0, 4), N(0, 3), &cap) == MOVE_ERR_GAME_OVER);
    CHECK(board_consistent(&b));
    board_destroy(&b);
}

/* ---- Concurrencia: 4 hilos pelean por la casilla (2,2) ---- */

typedef struct {
    Board *b;
    pthread_barrier_t *bar;
    int is_puma, id, from;
    MoveResult result;
} Racer;

static void *racer(void *arg) {
    Racer *r = arg;
    pthread_barrier_wait(r->bar);
    if (r->is_puma) r->result = board_move_puma(r->b, r->from, N(2, 2), NULL);
    else r->result = board_move_dog(r->b, r->id, r->from, N(2, 2));
    return NULL;
}

static void test_race(void) {
    printf("[race] 4 piezas disputan la misma casilla (2000 rondas)\n");
    int bad = 0;
    for (int iter = 0; iter < 2000; iter++) {
        Board b;
        board_init(&b);
        board_place_puma(&b, N(2, 1));
        board_place_dog(&b, 0, N(1, 2));
        board_place_dog(&b, 1, N(2, 3));
        board_place_dog(&b, 2, N(3, 2));
        pthread_barrier_t bar;
        pthread_barrier_init(&bar, NULL, 4);
        Racer rc[4] = {{&b, &bar, 1, 0, N(2, 1), MOVE_OK},
                       {&b, &bar, 0, 0, N(1, 2), MOVE_OK},
                       {&b, &bar, 0, 1, N(2, 3), MOVE_OK},
                       {&b, &bar, 0, 2, N(3, 2), MOVE_OK}};
        pthread_t th[4];
        for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, racer, &rc[i]);
        for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
        int ok = 0, occupied = 0;
        for (int i = 0; i < 4; i++) {
            if (rc[i].result == MOVE_OK) ok++;
            if (rc[i].result == MOVE_ERR_OCCUPIED) occupied++;
        }
        if (ok != 1 || occupied != 3 || !board_consistent(&b)) bad++;
        pthread_barrier_destroy(&bar);
        board_destroy(&b);
    }
    CHECK(bad == 0);
}

int main(void) {
    test_graph();
    test_moves();
    test_win_conditions();
    test_race();
    if (failures == 0) printf("\nTODO OK\n");
    else printf("\n%d FALLOS\n", failures);
    return failures != 0;
}
