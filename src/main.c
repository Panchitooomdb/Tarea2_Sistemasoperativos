#define _POSIX_C_SOURCE 200809L
#include "board.h"
#include "config.h"

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

int main(int argc, char **argv) {
    long n, m;
    if (argc < 3 || argc > 4 || parse_pos(argv[1], &n) != 0 || parse_pos(argv[2], &m) != 0) {
        fprintf(stderr, "Uso: %s N M [config.txt]\n  N = jugadores, M = mesas maximas\n", argv[0]);
        return 1;
    }

    Suite suite;
    int have_suite = 0;
    if (argc == 4) {
        char err[200];
        if (config_load(argv[3], &suite, err, sizeof err) != 0) {
            fprintf(stderr, "Error en '%s': %s\n", argv[3], err);
            return 1;
        }
        have_suite = 1;
        config_dump(&suite, stdout);
        printf("\n");
    }

    printf("[torneo] N=%ld jugadores, M=%ld mesas (matchmaker pendiente)\n", n, m);

    /* Demo del tablero: Puma en el centro */
    Board b;
    if (board_init(&b) != 0) {
        fprintf(stderr, "no se pudo inicializar el tablero\n");
        if (have_suite) config_free(&suite);
        return 1;
    }
    board_place_puma(&b, board_node_from_rc(2, 2));
    board_render(&b, stdout);
    board_destroy(&b);

    if (have_suite) config_free(&suite);
    return 0;
}
