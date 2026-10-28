#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "board.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOK 32

/* ---------- Tokenizador con cursor ---------- */

typedef struct {
    char *t[MAX_TOK];
    int n, i;
} Cur;

static void tokenize(Cur *c, char *line) {
    char *save = NULL;
    c->n = 0;
    c->i = 0;
    for (char *tok = strtok_r(line, " \t\r\n", &save); tok && c->n < MAX_TOK;
         tok = strtok_r(NULL, " \t\r\n", &save))
        c->t[c->n++] = tok;
}

static const char *next_tok(Cur *c) { return c->i < c->n ? c->t[c->i++] : NULL; }

static int expect(Cur *c, const char *kw) {
    const char *s = next_tok(c);
    return s != NULL && strcmp(s, kw) == 0;
}

static int to_int(const char *s, long *out) {
    char *end;
    if (!s || !*s) return -1;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || errno != 0) return -1;
    *out = v;
    return 0;
}

static int read_int(Cur *c, int *out) {
    long v;
    if (to_int(next_tok(c), &v) != 0 || v < -1000000 || v > 1000000000) return -1;
    *out = (int)v;
    return 0;
}

/* Un nodo es "TRONO_TOP|TRONO_LEFT|TRONO_RIGHT" o "fila columna".
 * Coordenadas fuera de 0..4 dan NODE_INVALID (se parsea bien, lo rechaza el tablero). */
static int read_node(Cur *c, int *out) {
    const char *s = next_tok(c);
    if (!s) return -1;
    if (strcmp(s, "TRONO_TOP") == 0) { *out = NODE_TOP; return 0; }
    if (strcmp(s, "TRONO_LEFT") == 0) { *out = NODE_LEFT; return 0; }
    if (strcmp(s, "TRONO_RIGHT") == 0) { *out = NODE_RIGHT; return 0; }
    long r;
    int col;
    if (to_int(s, &r) != 0 || read_int(c, &col) != 0) return -1;
    *out = (r < 0 || r > 1000) ? NODE_INVALID : board_node_from_rc((int)r, col);
    return 0;
}

/* ---------- Eventos ---------- */

#define BAD(msg) do { snprintf(why, n, "%s", (msg)); return -1; } while (0)

static int parse_event(Cur *c, Event *ev, char *why, size_t n) {
    const char *kw = next_tok(c);
    if (!kw) BAD("EVENT sin tipo");

    if (strcmp(kw, "MOVE") == 0) {
        const char *who = next_tok(c);
        ev->type = EV_MOVE;
        if (!who) BAD("MOVE sin pieza");
        if (strcmp(who, "PUMA") == 0) ev->is_puma = 1;
        else if (strcmp(who, "DOG") == 0) ev->is_puma = 0;
        else BAD("pieza desconocida (se esperaba PUMA o DOG)");
        if (read_int(c, &ev->piece_id) != 0) BAD("id de pieza invalido");
        if (!expect(c, "FROM") || read_node(c, &ev->from) != 0) BAD("se esperaba FROM <casilla>");
        if (!expect(c, "TO") || read_node(c, &ev->to) != 0) BAD("se esperaba TO <casilla>");
        const char *opt = next_tok(c);
        if (opt) {
            if (strcmp(opt, "CAPTURE") != 0) BAD("texto inesperado tras TO");
            if (!expect(c, "DOG") || read_int(c, &ev->capture_dog) != 0 || !expect(c, "AT") ||
                read_node(c, &ev->capture_at) != 0)
                BAD("se esperaba CAPTURE DOG <id> AT <casilla>");
            ev->has_capture = 1;
        }
        return 0;
    }
    if (strcmp(kw, "SPAWN_PLAYERS") == 0) {
        ev->type = EV_SPAWN_PLAYERS;
        const char *k;
        while ((k = next_tok(c)) != NULL) {
            if (strcmp(k, "COUNT") == 0) { if (read_int(c, &ev->count) != 0) BAD("COUNT invalido"); }
            else if (strcmp(k, "MESAS_MAX") == 0) { if (read_int(c, &ev->mesas_max) != 0) BAD("MESAS_MAX invalido"); }
            else BAD("parametro desconocido en SPAWN_PLAYERS");
        }
        return 0;
    }
    if (strcmp(kw, "WAIT_QUEUED_PLAYERS") == 0) { ev->type = EV_WAIT_QUEUED_PLAYERS; return 0; }
    if (strcmp(kw, "SIGNAL_SIGINT") == 0) { ev->type = EV_SIGINT; return 0; }
    if (strcmp(kw, "BURST_RANDOM_MOVES") == 0) {
        ev->type = EV_BURST_RANDOM_MOVES;
        if (!expect(c, "COUNT") || read_int(c, &ev->count) != 0) BAD("se esperaba COUNT <n>");
        return 0;
    }
    if (strcmp(kw, "DISCONNECT") == 0) {
        const char *who = next_tok(c);
        if (!who) BAD("DISCONNECT sin jugador");
        if (strcmp(who, "PLAYER_PUMA") == 0) { ev->type = EV_DISCONNECT_PUMA; return 0; }
        if (strcmp(who, "PLAYER_DOG") == 0) {
            ev->type = EV_DISCONNECT_DOG;
            if (!expect(c, "ID") || read_int(c, &ev->dog_id) != 0) BAD("se esperaba ID <n>");
            return 0;
        }
        BAD("jugador desconocido en DISCONNECT");
    }
    snprintf(why, n, "evento desconocido '%s'", kw);
    return -1;
}

/* ---------- Config ---------- */

static int parse_config_line(Cur *c, TestCase *tc, char *why, size_t n) {
    const char *k;
    while ((k = next_tok(c)) != NULL) {
        int v;
        if (strcmp(k, "SEED") == 0) {
            if (read_int(c, &v) != 0 || v < 0) BAD("SEED invalido");
            tc->seed = (unsigned int)v;
        } else if (strcmp(k, "DELAY_MIN") == 0) {
            if (read_int(c, &tc->delay_min) != 0 || tc->delay_min < 0) BAD("DELAY_MIN invalido");
        } else if (strcmp(k, "DELAY_MAX") == 0) {
            if (read_int(c, &tc->delay_max) != 0 || tc->delay_max < 0) BAD("DELAY_MAX invalido");
        } else if (strcmp(k, "PUMA_INIT") == 0) {
            if (read_node(c, &tc->puma_init) != 0) BAD("PUMA_INIT invalido");
        } else {
            snprintf(why, n, "clave desconocida '%s' en CONFIG", k);
            return -1;
        }
    }
    if (tc->delay_min > tc->delay_max) BAD("DELAY_MIN no puede ser mayor que DELAY_MAX");
    return 0;
}

/* ---------- Suite ---------- */

static TestCase *suite_add_case(Suite *s, int id, const char *name) {
    TestCase *p = realloc(s->cases, (size_t)(s->n_cases + 1) * sizeof *p);
    if (!p) return NULL;
    s->cases = p;
    TestCase *tc = &s->cases[s->n_cases++];
    memset(tc, 0, sizeof *tc);
    tc->id = id;
    snprintf(tc->name, sizeof tc->name, "%s", name);
    tc->seed = 1;           /* valores por defecto si el archivo no los define */
    tc->delay_min = 50;
    tc->delay_max = 300;
    tc->puma_init = NODE_TOP;
    return tc;
}

static int case_add_event(TestCase *tc, const Event *ev) {
    if (tc->n_events == tc->cap_events) {
        int ncap = tc->cap_events ? tc->cap_events * 2 : 8;
        Event *p = realloc(tc->events, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        tc->events = p;
        tc->cap_events = ncap;
    }
    tc->events[tc->n_events++] = *ev;
    return 0;
}

void config_free(Suite *s) {
    for (int i = 0; i < s->n_cases; i++) free(s->cases[i].events);
    free(s->cases);
    s->cases = NULL;
    s->n_cases = 0;
}

static void set_err(char *err, size_t errsz, int line, const char *msg) {
    snprintf(err, errsz, "linea %d: %s", line, msg);
}

int config_load(const char *path, Suite *suite, char *err, size_t errsz) {
    memset(suite, 0, sizeof *suite);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errsz, "no se pudo abrir '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[512], why[160];
    int lineno = 0, in_case = 0;
    int rc = 0;

    while (rc == 0 && fgets(line, sizeof line, f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) { set_err(err, errsz, lineno, "linea demasiado larga"); rc = -1; break; }

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;

        if (strncmp(p, "TEST_CASE", 9) == 0 && (p[9] == ' ' || p[9] == '\t')) {
            int id = 0;
            char name[96] = "";
            if (sscanf(p, "TEST_CASE %d \"%95[^\"]\"", &id, name) < 1) {
                set_err(err, errsz, lineno, "TEST_CASE invalido");
                rc = -1;
                break;
            }
            if (!suite_add_case(suite, id, name)) { set_err(err, errsz, lineno, "sin memoria"); rc = -1; break; }
            in_case = 1;
            continue;
        }

        Cur c;
        tokenize(&c, p);
        if (c.n == 0) continue;
        const char *kw = c.t[0];
        c.i = 1;

        if (strcmp(kw, "SUITE_START") == 0) {
            if (read_int(&c, &suite->declared) != 0) { set_err(err, errsz, lineno, "SUITE_START invalido"); rc = -1; }
        } else if (strcmp(kw, "SUITE_END") == 0) {
            break;
        } else if (strcmp(kw, "TEST_END") == 0) {
            int id;
            if (!in_case) { set_err(err, errsz, lineno, "TEST_END sin TEST_CASE"); rc = -1; }
            else if (read_int(&c, &id) != 0 || id != suite->cases[suite->n_cases - 1].id) {
                set_err(err, errsz, lineno, "TEST_END no coincide con el TEST_CASE abierto");
                rc = -1;
            } else in_case = 0;
        } else if (strcmp(kw, "CONFIG") == 0 || strcmp(kw, "EVENT") == 0) {
            if (!in_case) {                     /* archivo sin TEST_CASE: caso implicito */
                if (suite->n_cases > 0) {
                    set_err(err, errsz, lineno, "instruccion fuera de un TEST_CASE");
                    rc = -1;
                    break;
                }
                if (!suite_add_case(suite, 0, "(sin nombre)")) {
                    set_err(err, errsz, lineno, "sin memoria");
                    rc = -1;
                    break;
                }
                in_case = 1;
            }
            TestCase *tc = &suite->cases[suite->n_cases - 1];
            if (strcmp(kw, "CONFIG") == 0) {
                if (parse_config_line(&c, tc, why, sizeof why) != 0) { set_err(err, errsz, lineno, why); rc = -1; }
            } else {
                Event ev;
                memset(&ev, 0, sizeof ev);
                ev.from = ev.to = ev.capture_at = NODE_INVALID;
                ev.capture_dog = -1;
                ev.line = lineno;
                if (parse_event(&c, &ev, why, sizeof why) != 0) { set_err(err, errsz, lineno, why); rc = -1; }
                else if (case_add_event(tc, &ev) != 0) { set_err(err, errsz, lineno, "sin memoria"); rc = -1; }
            }
        } else {
            snprintf(why, sizeof why, "instruccion desconocida '%s'", kw);
            set_err(err, errsz, lineno, why);
            rc = -1;
        }
    }
    fclose(f);

    if (rc == 0 && suite->declared && suite->declared != suite->n_cases) {
        snprintf(err, errsz, "SUITE_START declara %d casos pero se leyeron %d", suite->declared, suite->n_cases);
        rc = -1;
    }
    if (rc != 0) config_free(suite);
    return rc;
}

/* ---------- Depuracion ---------- */

void config_dump(const Suite *s, FILE *out) {
    char a[24], b[24], d[24];
    fprintf(out, "Suite: %d casos (declarados: %d)\n", s->n_cases, s->declared);
    for (int i = 0; i < s->n_cases; i++) {
        const TestCase *tc = &s->cases[i];
        board_node_name(tc->puma_init, a, sizeof a);
        fprintf(out, "\n[Caso %d] %s\n  seed=%u delay=%d..%dms puma_init=%s eventos=%d\n", tc->id, tc->name,
                tc->seed, tc->delay_min, tc->delay_max, a, tc->n_events);
        for (int j = 0; j < tc->n_events; j++) {
            const Event *e = &tc->events[j];
            switch (e->type) {
            case EV_MOVE:
                board_node_name(e->from, a, sizeof a);
                board_node_name(e->to, b, sizeof b);
                fprintf(out, "  - MOVE %s %d: %s -> %s", e->is_puma ? "PUMA" : "DOG", e->piece_id, a, b);
                if (e->has_capture) {
                    board_node_name(e->capture_at, d, sizeof d);
                    fprintf(out, "  [captura DOG %d en %s]", e->capture_dog, d);
                }
                fprintf(out, "\n");
                break;
            case EV_SPAWN_PLAYERS: fprintf(out, "  - SPAWN_PLAYERS count=%d mesas_max=%d\n", e->count, e->mesas_max); break;
            case EV_WAIT_QUEUED_PLAYERS: fprintf(out, "  - WAIT_QUEUED_PLAYERS\n"); break;
            case EV_DISCONNECT_PUMA: fprintf(out, "  - DISCONNECT PLAYER_PUMA\n"); break;
            case EV_DISCONNECT_DOG: fprintf(out, "  - DISCONNECT PLAYER_DOG id=%d\n", e->dog_id); break;
            case EV_SIGINT: fprintf(out, "  - SIGNAL_SIGINT\n"); break;
            case EV_BURST_RANDOM_MOVES: fprintf(out, "  - BURST_RANDOM_MOVES count=%d\n", e->count); break;
            }
        }
    }
}
