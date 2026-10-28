#define _POSIX_C_SOURCE 200809L
#include "board.h"

#include <string.h>

#define MAX_NEIGH 6
#define MAX_JUMP 6

/* ---------- Grafo estatico (se construye una vez, despues es solo lectura) ---------- */

static int adj[NUM_NODES][MAX_NEIGH];
static int adj_n[NUM_NODES];
static int jmp_over[NUM_NODES][MAX_JUMP];
static int jmp_to[NUM_NODES][MAX_JUMP];
static int jmp_n[NUM_NODES];
static pthread_once_t graph_once = PTHREAD_ONCE_INIT;

static void add_edge(int a, int b) {
    adj[a][adj_n[a]++] = b;
    adj[b][adj_n[b]++] = a;
}

/* a-b-c colineales: se puede saltar de a a c sobre b, y de c a a */
static void add_line(int a, int b, int c) {
    jmp_over[a][jmp_n[a]] = b;
    jmp_to[a][jmp_n[a]++] = c;
    jmp_over[c][jmp_n[c]] = b;
    jmp_to[c][jmp_n[c]++] = a;
}

static void build_graph(void) {
    /* Cuadricula 5x5: solo horizontales y verticales, sin diagonales */
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int n = r * GRID_SIZE + c;
            if (c + 1 < GRID_SIZE) add_edge(n, n + 1);
            if (r + 1 < GRID_SIZE) add_edge(n, n + GRID_SIZE);
            if (c + 2 < GRID_SIZE) add_line(n, n + 1, n + 2);
            if (r + 2 < GRID_SIZE) add_line(n, n + GRID_SIZE, n + 2 * GRID_SIZE);
        }
    }
    /* Trono: triangulo TOP-LEFT-RIGHT y sus conexiones con la fila 0 */
    add_edge(NODE_TOP, NODE_LEFT);
    add_edge(NODE_TOP, NODE_RIGHT);
    add_edge(NODE_LEFT, NODE_RIGHT);
    add_edge(NODE_TOP, 2);   /* vertical central hasta (0,2) */
    add_edge(NODE_LEFT, 1);  /* vertical hasta (0,1) */
    add_edge(NODE_LEFT, 0);  /* diagonal hasta (0,0) */
    add_edge(NODE_RIGHT, 3); /* vertical hasta (0,3) */
    add_edge(NODE_RIGHT, 4); /* diagonal hasta (0,4) */
    /* Lineas de salto que entran o salen del trono */
    add_line(NODE_TOP, 2, 7);
    add_line(NODE_LEFT, 1, 6);
    add_line(NODE_RIGHT, 3, 8);
    add_line(NODE_TOP, NODE_LEFT, 0);
    add_line(NODE_TOP, NODE_RIGHT, 4);
    /* LEFT-RIGHT es una linea horizontal sin nodo intermedio: no hay salto */
}

static int valid_node(int n) { return n >= 0 && n < NUM_NODES; }

int board_adjacent(int a, int b) {
    pthread_once(&graph_once, build_graph);
    if (!valid_node(a) || !valid_node(b)) return 0;
    for (int i = 0; i < adj_n[a]; i++)
        if (adj[a][i] == b) return 1;
    return 0;
}

int board_jump_over(int from, int to) {
    pthread_once(&graph_once, build_graph);
    if (!valid_node(from) || !valid_node(to)) return -1;
    for (int i = 0; i < jmp_n[from]; i++)
        if (jmp_to[from][i] == to) return jmp_over[from][i];
    return -1;
}

int board_node_from_rc(int row, int col) {
    if (row < 0 || row >= GRID_SIZE || col < 0 || col >= GRID_SIZE) return NODE_INVALID;
    return row * GRID_SIZE + col;
}

void board_node_name(int node, char *buf, size_t sz) {
    if (node == NODE_TOP) snprintf(buf, sz, "TRONO_TOP");
    else if (node == NODE_LEFT) snprintf(buf, sz, "TRONO_LEFT");
    else if (node == NODE_RIGHT) snprintf(buf, sz, "TRONO_RIGHT");
    else if (node >= 0 && node < GRID_NODES) snprintf(buf, sz, "(%d,%d)", node / GRID_SIZE, node % GRID_SIZE);
    else snprintf(buf, sz, "INVALIDO");
}

const char *move_result_str(MoveResult r) {
    switch (r) {
    case MOVE_OK: return "OK";
    case MOVE_CAPTURE: return "CAPTURA";
    case MOVE_ERR_BAD_NODE: return "casilla inexistente";
    case MOVE_ERR_BAD_PIECE: return "pieza inexistente";
    case MOVE_ERR_NOT_AT_ORIGIN: return "la pieza no esta en el origen";
    case MOVE_ERR_OCCUPIED: return "casilla destino ocupada";
    case MOVE_ERR_NOT_ADJACENT: return "casillas no conectadas";
    case MOVE_ERR_NO_DOG_TO_JUMP: return "no hay perro que saltar";
    case MOVE_ERR_ALREADY_PLACED: return "pieza ya colocada";
    case MOVE_ERR_GAME_OVER: return "partida terminada";
    }
    return "?";
}

/* ---------- Ciclo de vida ---------- */

int board_init(Board *b) {
    pthread_once(&graph_once, build_graph);
    for (int i = 0; i < NUM_NODES; i++) b->cell[i] = CELL_EMPTY;
    b->puma_pos = -1;
    for (int i = 0; i < NUM_DOGS; i++) {
        b->dog_pos[i] = -1;
        b->dog_state[i] = DOG_UNPLACED;
    }
    b->dogs_captured = 0;
    return pthread_mutex_init(&b->lock, NULL);
}

void board_destroy(Board *b) { pthread_mutex_destroy(&b->lock); }

/* ---------- Helpers que asumen el lock tomado ---------- */

static int puma_has_moves_locked(const Board *b) {
    int p = b->puma_pos;
    for (int i = 0; i < adj_n[p]; i++)
        if (b->cell[adj[p][i]] == CELL_EMPTY) return 1;
    for (int i = 0; i < jmp_n[p]; i++)
        if (b->cell[jmp_to[p][i]] == CELL_EMPTY && b->cell[jmp_over[p][i]] >= 0) return 1;
    return 0;
}

static GameStatus status_locked(const Board *b) {
    if (b->dogs_captured >= CAPTURES_TO_WIN) return GAME_PUMA_WINS;
    if (b->puma_pos >= 0 && !puma_has_moves_locked(b)) return GAME_DOGS_WIN;
    return GAME_RUNNING;
}

/* ---------- Preparacion ---------- */

MoveResult board_place_puma(Board *b, int node) {
    MoveResult r;
    pthread_mutex_lock(&b->lock);
    if (!valid_node(node)) r = MOVE_ERR_BAD_NODE;
    else if (b->puma_pos != -1) r = MOVE_ERR_ALREADY_PLACED;
    else if (b->cell[node] != CELL_EMPTY) r = MOVE_ERR_OCCUPIED;
    else {
        b->cell[node] = CELL_PUMA;
        b->puma_pos = node;
        r = MOVE_OK;
    }
    pthread_mutex_unlock(&b->lock);
    return r;
}

MoveResult board_place_dog(Board *b, int id, int node) {
    MoveResult r;
    pthread_mutex_lock(&b->lock);
    if (id < 0 || id >= NUM_DOGS) r = MOVE_ERR_BAD_PIECE;
    else if (!valid_node(node)) r = MOVE_ERR_BAD_NODE;
    else if (b->dog_state[id] != DOG_UNPLACED) r = MOVE_ERR_ALREADY_PLACED;
    else if (b->cell[node] != CELL_EMPTY) r = MOVE_ERR_OCCUPIED;
    else {
        b->cell[node] = id;
        b->dog_pos[id] = node;
        b->dog_state[id] = DOG_ALIVE;
        r = MOVE_OK;
    }
    pthread_mutex_unlock(&b->lock);
    return r;
}

/* ---------- Movimientos ---------- */

MoveResult board_move_puma(Board *b, int from, int to, int *captured_dog) {
    MoveResult r;
    if (captured_dog) *captured_dog = -1;
    pthread_mutex_lock(&b->lock);
    if (!valid_node(from) || !valid_node(to)) r = MOVE_ERR_BAD_NODE;
    else if (status_locked(b) != GAME_RUNNING) r = MOVE_ERR_GAME_OVER;
    else if (b->puma_pos != from) r = MOVE_ERR_NOT_AT_ORIGIN;
    else if (board_adjacent(from, to)) {
        if (b->cell[to] != CELL_EMPTY) {
            r = MOVE_ERR_OCCUPIED;
        } else {
            b->cell[from] = CELL_EMPTY;
            b->cell[to] = CELL_PUMA;
            b->puma_pos = to;
            r = MOVE_OK;
        }
    } else {
        int over = board_jump_over(from, to);
        if (over < 0) r = MOVE_ERR_NOT_ADJACENT;
        else if (b->cell[over] < 0) r = MOVE_ERR_NO_DOG_TO_JUMP;
        else if (b->cell[to] != CELL_EMPTY) r = MOVE_ERR_OCCUPIED;
        else {
            int dog = b->cell[over];
            b->cell[over] = CELL_EMPTY;
            b->dog_state[dog] = DOG_CAPTURED;
            b->dog_pos[dog] = -1;
            b->dogs_captured++;
            b->cell[from] = CELL_EMPTY;
            b->cell[to] = CELL_PUMA;
            b->puma_pos = to;
            if (captured_dog) *captured_dog = dog;
            r = MOVE_CAPTURE;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return r;
}

MoveResult board_move_dog(Board *b, int id, int from, int to) {
    MoveResult r;
    pthread_mutex_lock(&b->lock);
    if (id < 0 || id >= NUM_DOGS) r = MOVE_ERR_BAD_PIECE;
    else if (!valid_node(from) || !valid_node(to)) r = MOVE_ERR_BAD_NODE;
    else if (status_locked(b) != GAME_RUNNING) r = MOVE_ERR_GAME_OVER;
    else if (b->dog_state[id] != DOG_ALIVE || b->dog_pos[id] != from) r = MOVE_ERR_NOT_AT_ORIGIN;
    else if (!board_adjacent(from, to)) r = MOVE_ERR_NOT_ADJACENT;
    else if (b->cell[to] != CELL_EMPTY) r = MOVE_ERR_OCCUPIED;
    else {
        b->cell[from] = CELL_EMPTY;
        b->cell[to] = id;
        b->dog_pos[id] = to;
        r = MOVE_OK;
    }
    pthread_mutex_unlock(&b->lock);
    return r;
}

/* ---------- Consultas ---------- */

GameStatus board_status(Board *b) {
    pthread_mutex_lock(&b->lock);
    GameStatus s = status_locked(b);
    pthread_mutex_unlock(&b->lock);
    return s;
}

int board_captures(Board *b) {
    pthread_mutex_lock(&b->lock);
    int n = b->dogs_captured;
    pthread_mutex_unlock(&b->lock);
    return n;
}

int board_consistent(Board *b) {
    int ok = 1, pumas = 0, captured = 0;
    pthread_mutex_lock(&b->lock);
    for (int n = 0; n < NUM_NODES; n++) {
        int v = b->cell[n];
        if (v == CELL_PUMA) {
            pumas++;
            if (b->puma_pos != n) ok = 0;
        } else if (v >= 0) {
            if (v >= NUM_DOGS || b->dog_state[v] != DOG_ALIVE || b->dog_pos[v] != n) ok = 0;
        } else if (v != CELL_EMPTY) {
            ok = 0;
        }
    }
    if (pumas > 1) ok = 0;
    if (b->puma_pos >= 0 && b->cell[b->puma_pos] != CELL_PUMA) ok = 0;
    if (b->puma_pos < 0 && pumas != 0) ok = 0;
    for (int d = 0; d < NUM_DOGS; d++) {
        if (b->dog_state[d] == DOG_ALIVE && b->cell[b->dog_pos[d]] != d) ok = 0;
        if (b->dog_state[d] == DOG_CAPTURED) captured++;
    }
    if (captured != b->dogs_captured) ok = 0;
    pthread_mutex_unlock(&b->lock);
    return ok;
}

void board_render(Board *b, FILE *out) {
    char s[NUM_NODES][8];
    int captured;
    pthread_mutex_lock(&b->lock);
    for (int n = 0; n < NUM_NODES; n++) {
        int v = b->cell[n];
        if (v == CELL_EMPTY) snprintf(s[n], sizeof s[n], ".");
        else if (v == CELL_PUMA) snprintf(s[n], sizeof s[n], "P");
        else snprintf(s[n], sizeof s[n], "d%d", v);
    }
    captured = b->dogs_captured;
    pthread_mutex_unlock(&b->lock);

    fprintf(out, "            [%-3s]\n", s[NODE_TOP]);
    fprintf(out, "       [%-3s]     [%-3s]\n", s[NODE_LEFT], s[NODE_RIGHT]);
    for (int r = 0; r < GRID_SIZE; r++) {
        fprintf(out, "   ");
        for (int c = 0; c < GRID_SIZE; c++) fprintf(out, "[%-3s]", s[r * GRID_SIZE + c]);
        fprintf(out, "\n");
    }
    fprintf(out, "   capturas: %d/%d\n", captured, CAPTURES_TO_WIN);
}
