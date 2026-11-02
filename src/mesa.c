#define _POSIX_C_SOURCE 200809L
#include "mesa.h"

#include "board.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define NUM_SLOTS (1 + NUM_DOGS)
#define INTENTOS_DEFECTO 600
#define JUEZ_POLL_MS 50
#define MAX_CAND 8

typedef enum { LOG_MOVE, LOG_ABANDONO } LogKind;

typedef struct LogNode {
    LogKind kind;
    long t_ms;
    int is_puma, piece_id, from, to;
    MoveResult res;
    int captured;      /* id del perro capturado o -1 */
    int expect_cap;
    struct LogNode *next;
} LogNode;

typedef struct Mesa Mesa;

typedef struct {
    Mesa *m;
    int slot;
    unsigned int rng;
    const Event **acts;
    int n_acts;
    int disconnect;
    int created;
    pthread_t th;
} Piece;

struct Mesa {
    int id;
    const TestCase *tc;
    atomic_int *apagado;
    unsigned int seed;
    int dmin, dmax;
    Board board;

    pthread_mutex_t mu;
    pthread_cond_t q_cv, sleep_cv;
    LogNode *head, *tail;
    int stop;                 /* el Juez termina cuando stop y la cola esta vacia */

    atomic_int done;
    MesaFin fin;              /* protegido por mu; lo fija el primero que llama finish */
    atomic_int budget;        /* intentos restantes en modo libre */
    atomic_int active;        /* hilos guionizados aun vivos */
    atomic_int st_ok, st_rej, st_cap;
    struct timespec t0;
};

/* ---------- utilidades ---------- */

static unsigned int rng_next(unsigned int *s) {
    unsigned int x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void shuffle(int *a, int n, unsigned int *rng) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rng_next(rng) % (unsigned int)(i + 1));
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static int rand_delay(Piece *p) {
    int span = p->m->dmax - p->m->dmin + 1;
    return p->m->dmin + (int)(rng_next(&p->rng) % (unsigned int)span);
}

static long now_ms(const Mesa *m) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - m->t0.tv_sec) * 1000L + (t.tv_nsec - m->t0.tv_nsec) / 1000000L;
}

static const char *fin_str(MesaFin f) {
    switch (f) {
    case MESA_FIN_PUMA_GANA: return "gana el PUMA (6 capturas)";
    case MESA_FIN_PERROS_GANAN: return "ganan los PERROS (puma acorralado)";
    case MESA_FIN_ABANDONO_PUMA: return "ganan los PERROS por abandono del Puma";
    case MESA_FIN_ABANDONO_PERROS: return "gana el PUMA por abandono de los Perros";
    case MESA_FIN_GUION: return "guion completo (sin ganador)";
    case MESA_FIN_LIMITE: return "limite de intentos (sin ganador)";
    case MESA_FIN_APAGADO: return "apagado ordenado (SIGINT)";
    case MESA_FIN_ERROR: return "error interno";
    }
    return "?";
}

/* ---------- fin de partida y cola de eventos ---------- */

static void finish_locked(Mesa *m, MesaFin f) {
    if (!atomic_exchange(&m->done, 1)) m->fin = f;
    pthread_cond_broadcast(&m->q_cv);
    pthread_cond_broadcast(&m->sleep_cv);
}

static void finish(Mesa *m, MesaFin f) {
    pthread_mutex_lock(&m->mu);
    finish_locked(m, f);
    pthread_mutex_unlock(&m->mu);
}

static void push_node(Mesa *m, LogNode *n, int finish_with, int do_finish) {
    pthread_mutex_lock(&m->mu);
    n->next = NULL;
    if (m->tail) m->tail->next = n;
    else m->head = n;
    m->tail = n;
    if (do_finish) finish_locked(m, (MesaFin)finish_with);
    pthread_cond_signal(&m->q_cv);
    pthread_mutex_unlock(&m->mu);
}

static void record(Mesa *m, int is_puma, int id, int from, int to, MoveResult r, int cap, int expect_cap) {
    if (r == MOVE_OK || r == MOVE_CAPTURE) atomic_fetch_add(&m->st_ok, 1);
    else atomic_fetch_add(&m->st_rej, 1);
    if (r == MOVE_CAPTURE) atomic_fetch_add(&m->st_cap, 1);

    LogNode *n = calloc(1, sizeof *n);
    if (!n) return; /* sin memoria: se pierde la linea de log, el estado sigue siendo correcto */
    n->kind = LOG_MOVE;
    n->t_ms = now_ms(m);
    n->is_puma = is_puma;
    n->piece_id = id;
    n->from = from;
    n->to = to;
    n->res = r;
    n->captured = cap;
    n->expect_cap = expect_cap;
    push_node(m, n, 0, 0);
}

static void abandon(Mesa *m, int puma_abandons) {
    LogNode *n = calloc(1, sizeof *n);
    if (n) {
        n->kind = LOG_ABANDONO;
        n->t_ms = now_ms(m);
        n->is_puma = puma_abandons;
        n->captured = -1;
        push_node(m, n, puma_abandons ? MESA_FIN_ABANDONO_PUMA : MESA_FIN_ABANDONO_PERROS, 1);
    } else {
        finish(m, puma_abandons ? MESA_FIN_ABANDONO_PUMA : MESA_FIN_ABANDONO_PERROS);
    }
}

/* Espera ms milisegundos o hasta que termine la partida. Devuelve 1 si la partida termino. */
static int sleep_ms(Mesa *m, int ms) {
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&m->mu);
    while (!atomic_load(&m->done)) {
        if (pthread_cond_timedwait(&m->sleep_cv, &m->mu, &dl) == ETIMEDOUT) break;
    }
    int d = atomic_load(&m->done);
    pthread_mutex_unlock(&m->mu);
    return d;
}

/* ---------- Juez ---------- */

static void print_entry(Mesa *m, const LogNode *e) {
    char a[24], b[24], c[24], who[24];
    if (e->kind == LOG_ABANDONO) {
        printf("[mesa %d] t=%5ldms  ABANDONO: se desconecta el jugador %s\n", m->id, e->t_ms,
               e->is_puma ? "Puma" : "Perros");
        return;
    }
    if (e->res == MOVE_ERR_GAME_OVER) return; /* ruido tras terminar la partida */
    board_node_name(e->from, a, sizeof a);
    board_node_name(e->to, b, sizeof b);
    if (e->is_puma) snprintf(who, sizeof who, "Puma");
    else snprintf(who, sizeof who, "Perro %d", e->piece_id);

    if (e->res == MOVE_OK || e->res == MOVE_CAPTURE) {
        printf("[mesa %d] t=%5ldms  %-8s %s -> %s  %s", m->id, e->t_ms, who, a, b,
               e->res == MOVE_CAPTURE ? "CAPTURA" : "OK");
        if (e->res == MOVE_CAPTURE) {
            board_node_name(board_jump_over(e->from, e->to), c, sizeof c);
            printf(" (perro %d en %s)", e->captured, c);
        }
        printf("\n");
        board_render(&m->board, stdout);
    } else {
        printf("[mesa %d] t=%5ldms  %-8s %s -> %s  RECHAZADO (%s)\n", m->id, e->t_ms, who, a, b,
               move_result_str(e->res));
    }
    if (e->expect_cap && e->res != MOVE_CAPTURE) printf("[mesa %d]   aviso: el evento esperaba una captura\n", m->id);
}

static void check_apagado(Mesa *m) {
    if (m->apagado && atomic_load(m->apagado) && !atomic_load(&m->done)) finish(m, MESA_FIN_APAGADO);
}

static void *judge_main(void *arg) {
    Mesa *m = arg;
    for (;;) {
        pthread_mutex_lock(&m->mu);
        while (!m->head && !m->stop) {
            struct timespec dl;
            clock_gettime(CLOCK_MONOTONIC, &dl);
            dl.tv_nsec += JUEZ_POLL_MS * 1000000L;
            if (dl.tv_nsec >= 1000000000L) {
                dl.tv_sec++;
                dl.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&m->q_cv, &m->mu, &dl);
            if (m->apagado && atomic_load(m->apagado)) finish_locked(m, MESA_FIN_APAGADO);
        }
        LogNode *e = m->head;
        if (!e) { /* stop y cola vacia: ya no llegara nada mas */
            pthread_mutex_unlock(&m->mu);
            break;
        }
        m->head = e->next;
        if (!m->head) m->tail = NULL;
        pthread_mutex_unlock(&m->mu);

        print_entry(m, e);
        free(e);

        if (!atomic_load(&m->done)) {
            GameStatus s = board_status(&m->board);
            if (s == GAME_PUMA_WINS) finish(m, MESA_FIN_PUMA_GANA);
            else if (s == GAME_DOGS_WIN) finish(m, MESA_FIN_PERROS_GANAN);
        }
        check_apagado(m);
    }
    return NULL;
}

/* ---------- Piezas: modo guionizado ---------- */

static void run_event(Piece *p, const Event *ev) {
    Mesa *m = p->m;
    int cap = -1;
    MoveResult r;
    if (ev->is_puma) r = board_move_puma(&m->board, ev->from, ev->to, &cap);
    else r = board_move_dog(&m->board, ev->piece_id, ev->from, ev->to);
    record(m, ev->is_puma, ev->piece_id, ev->from, ev->to, r, cap, ev->has_capture);
}

static void *scripted_main(void *arg) {
    Piece *p = arg;
    Mesa *m = p->m;
    for (int i = 0; i < p->n_acts; i++) {
        if (sleep_ms(m, rand_delay(p))) break;
        run_event(p, p->acts[i]);
    }
    if (p->disconnect && !atomic_load(&m->done) && !sleep_ms(m, rand_delay(p))) abandon(m, p->slot == 0);
    if (atomic_fetch_sub(&m->active, 1) == 1) finish(m, MESA_FIN_GUION);
    return NULL;
}

/* ---------- Piezas: modo libre ---------- */

static void puma_step(Piece *p) {
    Mesa *m = p->m;
    int pos = board_puma_pos(&m->board);
    if (pos < 0) return;
    int over[MAX_CAND], to[MAX_CAND], nb[MAX_CAND];
    int nj = board_jumps(pos, over, to, MAX_CAND);
    int nn = board_neighbors(pos, nb, MAX_CAND);
    int ij[MAX_CAND], in[MAX_CAND];
    for (int i = 0; i < nj; i++) ij[i] = i;
    for (int i = 0; i < nn; i++) in[i] = i;
    shuffle(ij, nj, &p->rng);
    shuffle(in, nn, &p->rng);

    for (int k = 0; k < nj; k++) { /* el Puma prefiere capturar */
        int cap = -1;
        int dst = to[ij[k]];
        MoveResult r = board_move_puma(&m->board, pos, dst, &cap);
        if (r == MOVE_CAPTURE) { record(m, 1, 0, pos, dst, r, cap, 0); return; }
        if (r == MOVE_ERR_GAME_OVER) return;
    }
    for (int k = 0; k < nn; k++) {
        int dst = nb[in[k]];
        MoveResult r = board_move_puma(&m->board, pos, dst, NULL);
        if (r == MOVE_OK) { record(m, 1, 0, pos, dst, r, -1, 0); return; }
        if (r == MOVE_ERR_GAME_OVER) return;
    }
    atomic_fetch_add(&m->st_rej, 1); /* sin salida este turno */
}

static int dog_step(Piece *p) { /* 0 si el perro ya no esta en juego */
    Mesa *m = p->m;
    int id = p->slot - 1;
    int pos = board_dog_pos(&m->board, id);
    if (pos < 0) return 0;
    int nb[MAX_CAND], idx[MAX_CAND];
    int nn = board_neighbors(pos, nb, MAX_CAND);
    for (int i = 0; i < nn; i++) idx[i] = i;
    shuffle(idx, nn, &p->rng);
    for (int k = 0; k < nn; k++) {
        int dst = nb[idx[k]];
        MoveResult r = board_move_dog(&m->board, id, pos, dst);
        if (r == MOVE_OK) { record(m, 0, id, pos, dst, r, -1, 0); return 1; }
        if (r == MOVE_ERR_GAME_OVER) return 1;
    }
    atomic_fetch_add(&m->st_rej, 1);
    return 1;
}

static void *free_main(void *arg) {
    Piece *p = arg;
    Mesa *m = p->m;
    while (!atomic_load(&m->done)) {
        if (sleep_ms(m, rand_delay(p))) break;
        if (atomic_fetch_sub(&m->budget, 1) <= 0) {
            finish(m, MESA_FIN_LIMITE);
            break;
        }
        if (p->slot == 0) puma_step(p);
        else if (!dog_step(p)) break;
    }
    return NULL;
}

/* ---------- Preparacion ---------- */

static void place_default_dogs(Mesa *m) {
    /* filas 3 y 4 (nodos 15..24) para los perros 0..9, y (2,0) y (2,4) para el 10 y 11 */
    for (int id = 0; id < NUM_DOGS; id++) {
        int node = id < 10 ? 15 + id : (id == 10 ? 10 : 14);
        if (board_place_dog(&m->board, id, node) != MOVE_OK)
            for (int n = GRID_NODES - 1; n >= 0; n--)
                if (board_place_dog(&m->board, id, n) == MOVE_OK) break;
    }
}

static int has_script(const TestCase *tc) {
    if (!tc) return 0;
    for (int i = 0; i < tc->n_events; i++)
        if (tc->events[i].type == EV_MOVE || tc->events[i].type == EV_DISCONNECT_PUMA ||
            tc->events[i].type == EV_DISCONNECT_DOG)
            return 1;
    return 0;
}

static void say_place(Mesa *m, const char *who, int node, MoveResult r) {
    char a[24];
    board_node_name(node, a, sizeof a);
    if (r == MOVE_OK) printf("[mesa %d] colocacion: %s en %s\n", m->id, who, a);
    else printf("[mesa %d] colocacion RECHAZADA: %s en %s (%s)\n", m->id, who, a, move_result_str(r));
}

/* ---------- API ---------- */

int mesa_jugar_info(int id_mesa, const TestCase *tc, atomic_int *apagado, MesaResultado *out) {
    Mesa m;
    Piece pieces[NUM_SLOTS];
    pthread_t judge;
    int rc = 0;
    memset(&m, 0, sizeof m);
    memset(pieces, 0, sizeof pieces);

    m.id = id_mesa;
    m.tc = tc;
    m.apagado = apagado;
    m.seed = tc ? tc->seed : 1;
    m.dmin = tc ? tc->delay_min : 50;
    m.dmax = tc ? tc->delay_max : 300;
    if (m.dmax < m.dmin) m.dmax = m.dmin;
    atomic_init(&m.done, 0);
    atomic_init(&m.budget, INTENTOS_DEFECTO);
    atomic_init(&m.active, 0);
    atomic_init(&m.st_ok, 0);
    atomic_init(&m.st_rej, 0);
    atomic_init(&m.st_cap, 0);
    m.fin = MESA_FIN_ERROR;
    clock_gettime(CLOCK_MONOTONIC, &m.t0);

    if (board_init(&m.board) != 0) return -1;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    if (pthread_mutex_init(&m.mu, NULL) != 0 || pthread_cond_init(&m.q_cv, &ca) != 0 ||
        pthread_cond_init(&m.sleep_cv, &ca) != 0) {
        pthread_condattr_destroy(&ca);
        board_destroy(&m.board);
        return -1;
    }
    pthread_condattr_destroy(&ca);

    int scripted = has_script(tc);
    int puma_node = tc ? tc->puma_init : NODE_TOP;
    say_place(&m, "Puma", puma_node, board_place_puma(&m.board, puma_node));

    /* piezas: semillas derivadas de la semilla del config, el id de mesa y el slot */
    for (int s = 0; s < NUM_SLOTS; s++) {
        pieces[s].m = &m;
        pieces[s].slot = s;
        unsigned int sd = m.seed * 2654435761u + (unsigned int)(id_mesa + 1) * 40503u + (unsigned int)(s + 1) * 9973u;
        pieces[s].rng = sd | 1u;
        for (int k = 0; k < 4; k++) rng_next(&pieces[s].rng);
    }

    if (scripted) {
        int count[NUM_SLOTS] = {0};
        int tried[NUM_SLOTS] = {0};
        /* pasada 1: colocar piezas (primera aparicion en su FROM) y contar acciones */
        for (int j = 0; j < tc->n_events; j++) {
            const Event *ev = &tc->events[j];
            if (ev->type == EV_DISCONNECT_PUMA) { pieces[0].disconnect = 1; continue; }
            if (ev->type == EV_DISCONNECT_DOG) {
                if (ev->dog_id >= 0 && ev->dog_id < NUM_DOGS) pieces[1 + ev->dog_id].disconnect = 1;
                else printf("[mesa %d] linea %d: perro %d inexistente, DISCONNECT ignorado\n", m.id, ev->line, ev->dog_id);
                continue;
            }
            if (ev->type != EV_MOVE) continue; /* SPAWN/WAIT/SIGINT/BURST: los maneja el torneo */
            int slot = ev->is_puma ? 0 : 1 + ev->piece_id;
            if (!ev->is_puma && (ev->piece_id < 0 || ev->piece_id >= NUM_DOGS)) {
                printf("[mesa %d] linea %d: perro %d inexistente, evento ignorado\n", m.id, ev->line, ev->piece_id);
                continue;
            }
            if (!ev->is_puma && !tried[slot]) {
                char who[24];
                snprintf(who, sizeof who, "Perro %d", ev->piece_id);
                tried[slot] = 1;
                say_place(&m, who, ev->from, board_place_dog(&m.board, ev->piece_id, ev->from));
            }
            if (ev->from == ev->to) continue; /* solo colocacion */
            count[slot]++;
        }
        for (int s = 0; s < NUM_SLOTS; s++) {
            if (count[s] > 0) {
                pieces[s].acts = calloc((size_t)count[s], sizeof *pieces[s].acts);
                if (!pieces[s].acts) { rc = -1; count[s] = 0; }
            }
        }
        /* pasada 2: repartir eventos por pieza, en el orden del archivo */
        for (int j = 0; j < tc->n_events; j++) {
            const Event *ev = &tc->events[j];
            if (ev->type != EV_MOVE || ev->from == ev->to) continue;
            if (!ev->is_puma && (ev->piece_id < 0 || ev->piece_id >= NUM_DOGS)) continue;
            int slot = ev->is_puma ? 0 : 1 + ev->piece_id;
            if (pieces[slot].acts && pieces[slot].n_acts < count[slot]) pieces[slot].acts[pieces[slot].n_acts++] = ev;
        }
    } else {
        place_default_dogs(&m);
        if (tc)
            for (int j = 0; j < tc->n_events; j++)
                if (tc->events[j].type == EV_BURST_RANDOM_MOVES && tc->events[j].count > 0)
                    atomic_store(&m.budget, tc->events[j].count);
    }

    printf("[mesa %d] partida iniciada (%s, seed=%u, retardo %d..%d ms)\n", m.id,
           scripted ? "guionizada" : "juego libre", m.seed, m.dmin, m.dmax);
    board_render(&m.board, stdout);

    if (rc != 0 || pthread_create(&judge, NULL, judge_main, &m) != 0) {
        fprintf(stderr, "[mesa %d] no se pudo iniciar el juez\n", m.id);
        for (int s = 0; s < NUM_SLOTS; s++) free(pieces[s].acts);
        pthread_cond_destroy(&m.q_cv);
        pthread_cond_destroy(&m.sleep_cv);
        pthread_mutex_destroy(&m.mu);
        board_destroy(&m.board);
        return -1;
    }

    /* lanzar piezas */
    int planned = 0;
    for (int s = 0; s < NUM_SLOTS; s++)
        if (!scripted || pieces[s].n_acts > 0 || pieces[s].disconnect) planned++;
    atomic_store(&m.active, planned);
    if (planned == 0) finish(&m, MESA_FIN_GUION); /* solo habia colocaciones */
    for (int s = 0; s < NUM_SLOTS; s++) {
        if (scripted && pieces[s].n_acts == 0 && !pieces[s].disconnect) continue;
        void *(*fn)(void *) = scripted ? scripted_main : free_main;
        if (pthread_create(&pieces[s].th, NULL, fn, &pieces[s]) == 0) {
            pieces[s].created = 1;
        } else {
            fprintf(stderr, "[mesa %d] no se pudo crear el hilo de la pieza %d\n", m.id, s);
            finish(&m, MESA_FIN_ERROR);
            rc = -1;
        }
    }

    for (int s = 0; s < NUM_SLOTS; s++)
        if (pieces[s].created) pthread_join(pieces[s].th, NULL);

    /* ya no llegan eventos: el Juez vacia la cola y termina */
    pthread_mutex_lock(&m.mu);
    m.stop = 1;
    pthread_cond_broadcast(&m.q_cv);
    pthread_mutex_unlock(&m.mu);
    pthread_join(judge, NULL);

    if (!atomic_load(&m.done)) finish(&m, MESA_FIN_GUION);
    printf("[mesa %d] FIN: %s | movimientos ok=%d rechazados=%d capturas=%d | %ld ms\n", m.id, fin_str(m.fin),
           atomic_load(&m.st_ok), atomic_load(&m.st_rej), atomic_load(&m.st_cap), now_ms(&m));
    board_render(&m.board, stdout);

    if (out) {
        out->fin = m.fin;
        out->mov_ok = atomic_load(&m.st_ok);
        out->mov_rechazados = atomic_load(&m.st_rej);
        out->capturas = atomic_load(&m.st_cap);
        out->tablero_consistente = board_consistent(&m.board);
    }

    for (int s = 0; s < NUM_SLOTS; s++) free(pieces[s].acts);
    pthread_cond_destroy(&m.q_cv);
    pthread_cond_destroy(&m.sleep_cv);
    pthread_mutex_destroy(&m.mu);
    board_destroy(&m.board);
    return rc;
}

int mesa_jugar(int id_mesa, const TestCase *tc, atomic_int *apagado) {
    return mesa_jugar_info(id_mesa, tc, apagado, NULL);
}
