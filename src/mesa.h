#ifndef MESA_H
#define MESA_H

#include <stdatomic.h>
#include "config.h"

/* Juega una partida completa en la mesa id_mesa.
 * Bloquea hasta que termina (victoria, abandono o apagado).
 * Si *apagado vale 1 (Ctrl+C), debe terminar de forma ordenada.
 * Devuelve 0 si todo salio bien. */
int mesa_jugar(int id_mesa, const TestCase *tc, atomic_int *apagado);

/* Igual que mesa_jugar, pero entrega ademas el resultado de la partida
 * (util para pruebas y para el resumen del torneo). out puede ser NULL. */
typedef enum {
    MESA_FIN_PUMA_GANA,        /* 6 capturas */
    MESA_FIN_PERROS_GANAN,     /* puma acorralado */
    MESA_FIN_ABANDONO_PUMA,    /* se desconecta el Puma: ganan los Perros */
    MESA_FIN_ABANDONO_PERROS,  /* se desconectan los Perros: gana el Puma */
    MESA_FIN_GUION,            /* se ejecutaron todos los eventos del archivo */
    MESA_FIN_LIMITE,           /* se agoto el presupuesto de intentos (juego libre) */
    MESA_FIN_APAGADO,          /* SIGINT */
    MESA_FIN_ERROR
} MesaFin;

typedef struct {
    MesaFin fin;
    int mov_ok;
    int mov_rechazados;
    int capturas;
    int tablero_consistente;
} MesaResultado;

int mesa_jugar_info(int id_mesa, const TestCase *tc, atomic_int *apagado, MesaResultado *out);

#endif
