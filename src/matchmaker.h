#ifndef MATCHMAKER_H
#define MATCHMAKER_H

#include "config.h"

/* Simula n jugadores que llegan, los empareja de a 2 y los asigna a
 * a lo mas m mesas simultaneas. tc puede ser NULL (sin config).
 * Devuelve 0 si todo salio bien. */
int torneo_run(int n, int m, const TestCase *tc);

#endif
