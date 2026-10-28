#ifndef BOARD_H
#define BOARD_H

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Modelo del tablero del Komikan: grafo de 28 nodos.
 *   0..24 : cuadricula 5x5, nodo = fila*5 + columna
 *   25    : TRONO_TOP
 *   26    : TRONO_LEFT
 *   27    : TRONO_RIGHT
 * Las conexiones y las lineas de salto estan en tablas (board.c), derivadas
 * del dibujo del enunciado. Si el ayudante cambia la geometria, solo se
 * modifica build_graph().
 */

#define GRID_SIZE 5
#define GRID_NODES 25
#define NODE_TOP 25
#define NODE_LEFT 26
#define NODE_RIGHT 27
#define NUM_NODES 28
#define NODE_INVALID (-1)

#define NUM_DOGS 12
#define CAPTURES_TO_WIN 6

#define CELL_EMPTY (-1)
#define CELL_PUMA (-2) /* un valor >= 0 en una celda es el id del perro */

typedef enum {
    MOVE_OK = 0,
    MOVE_CAPTURE,
    MOVE_ERR_BAD_NODE,
    MOVE_ERR_BAD_PIECE,
    MOVE_ERR_NOT_AT_ORIGIN,
    MOVE_ERR_OCCUPIED,
    MOVE_ERR_NOT_ADJACENT,
    MOVE_ERR_NO_DOG_TO_JUMP,
    MOVE_ERR_ALREADY_PLACED,
    MOVE_ERR_GAME_OVER
} MoveResult;

typedef enum { GAME_RUNNING = 0, GAME_PUMA_WINS, GAME_DOGS_WIN } GameStatus;

typedef enum { DOG_UNPLACED = 0, DOG_ALIVE, DOG_CAPTURED } DogState;

typedef struct {
    int cell[NUM_NODES];     /* CELL_EMPTY, CELL_PUMA o id de perro */
    int puma_pos;            /* -1 si aun no esta colocado */
    int dog_pos[NUM_DOGS];   /* -1 si no esta en el tablero */
    DogState dog_state[NUM_DOGS];
    int dogs_captured;
    pthread_mutex_t lock;    /* protege TODO el estado de arriba */
} Board;

/* Grafo (solo lectura, no necesita lock) */
int board_adjacent(int a, int b);
int board_jump_over(int from, int to); /* nodo intermedio de un salto o -1 */
int board_node_from_rc(int row, int col); /* NODE_INVALID si esta fuera */
void board_node_name(int node, char *buf, size_t sz);
const char *move_result_str(MoveResult r);

/* Ciclo de vida */
int board_init(Board *b);     /* 0 = ok */
void board_destroy(Board *b);

/* Preparacion (setup): NO valida adyacencia, solo que la celda este libre */
MoveResult board_place_puma(Board *b, int node);
MoveResult board_place_dog(Board *b, int id, int node);

/* Movimientos: cada uno es una seccion critica completa (validar + aplicar) */
MoveResult board_move_puma(Board *b, int from, int to, int *captured_dog);
MoveResult board_move_dog(Board *b, int id, int from, int to);

/* Consultas */
GameStatus board_status(Board *b);
int board_captures(Board *b);
int board_consistent(Board *b); /* 1 si cell[] y posiciones concuerdan */
void board_render(Board *b, FILE *out);

#endif
