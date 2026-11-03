#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "matchmaker.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_pos(const char *s, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno != 0 || v <= 0 || v > 100000) return -1;
    *out = v;
    return 0;
}

/* Devuelve el evento SPAWN_PLAYERS del caso, o NULL si no tiene. */
static const Event *buscar_spawn(const TestCase *tc) {
    for (int i = 0; i < tc->n_events; i++)
        if (tc->events[i].type == EV_SPAWN_PLAYERS) return &tc->events[i];
    return NULL;
}

int main(int argc, char **argv) {
    long n, m, solo = 0;
    if (argc < 3 || argc > 5 || parse_pos(argv[1], &n) != 0 ||
        parse_pos(argv[2], &m) != 0 || (argc == 5 && parse_pos(argv[4], &solo) != 0)) {
        fprintf(stderr,
                "Uso: %s N M [config.txt [caso]]\n"
                "  N = jugadores, M = mesas maximas\n"
                "  caso = (opcional) ejecuta solo ese numero de caso del archivo\n",
                argv[0]);
        return 1;
    }
    if (argc == 3) {                       /* sin config: juego libre */
        printf("[torneo] N=%ld jugadores, M=%ld mesas\n", n, m);
        return torneo_run((int)n, (int)m, NULL);
    }
    if (argc == 5 && solo == 0) return 1;

    Suite suite;
    char err[200];
    if (config_load(argv[3], &suite, err, sizeof err) != 0) {
        fprintf(stderr, "Error en '%s': %s\n", argv[3], err);
        return 1;
    }

    int rc = 0, ejecutados = 0;
    for (int i = 0; i < suite.n_cases; i++) {
        const TestCase *tc = &suite.cases[i];
        if (solo && tc->id != solo) continue;
        const Event *sp = buscar_spawn(tc);
        int n_caso = sp ? sp->count : 2;      /* sin SPAWN: una partida */
        int m_caso = sp ? sp->mesas_max : 1;
        printf("\n===== CASO %d: %s =====\n", tc->id, tc->name);
        if (n_caso <= 0 || m_caso <= 0) {
            fprintf(stderr, "caso %d: SPAWN_PLAYERS invalido\n", tc->id);
            rc = 1;
            continue;
        }
        printf("[torneo] N=%d jugadores, M=%d mesas\n", n_caso, m_caso);
        if (torneo_run(n_caso, m_caso, tc) != 0) rc = 1;
        ejecutados++;
    }
    if (ejecutados == 0) {
        fprintf(stderr, "no se ejecuto ningun caso\n");
        rc = 1;
    }
    config_free(&suite);
    return rc;
}
