#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include "mesa.h"

/* VERSION DE PRUEBA: se borra cuando exista la mesa real de Francisco. */
int mesa_jugar(int id_mesa, const TestCase *tc, atomic_int *apagado) {
    (void)tc;
    printf("[mesa %d] partida iniciada\n", id_mesa);
    for (int i = 0; i < 20 && !atomic_load(apagado); i++) {
        struct timespec ts = {0, 100 * 1000000L};   /* 100 ms */
        nanosleep(&ts, NULL);
    }
    printf("[mesa %d] partida terminada\n", id_mesa);
    return 0;
}
