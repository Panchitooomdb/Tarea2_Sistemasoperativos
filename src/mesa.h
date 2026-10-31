#ifndef MESA_H
#define MESA_H

#include <stdatomic.h>
#include "config.h"

/* Juega una partida completa en la mesa id_mesa.
 * Bloquea hasta que termina (victoria, abandono o apagado).
 * Si *apagado vale 1 (Ctrl+C), debe terminar de forma ordenada.
 * Devuelve 0 si todo salio bien. */
int mesa_jugar(int id_mesa, const TestCase *tc, atomic_int *apagado);

#endif
