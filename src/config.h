#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdio.h>

typedef enum {
    EV_MOVE,
    EV_SPAWN_PLAYERS,
    EV_WAIT_QUEUED_PLAYERS,
    EV_DISCONNECT_PUMA,
    EV_DISCONNECT_DOG,
    EV_SIGINT,
    EV_BURST_RANDOM_MOVES
} EventType;

typedef struct {
    EventType type;
    int line;          /* linea del archivo, para mensajes */
    /* EV_MOVE */
    int is_puma;       /* 1 = Puma, 0 = Perro */
    int piece_id;
    int from, to;      /* nodos (NODE_INVALID si las coordenadas estan fuera) */
    int has_capture;
    int capture_dog;
    int capture_at;
    /* EV_SPAWN_PLAYERS / EV_BURST_RANDOM_MOVES */
    int count;
    int mesas_max;
    /* EV_DISCONNECT_DOG */
    int dog_id;
} Event;

typedef struct {
    int id;
    char name[96];
    unsigned int seed;
    int delay_min, delay_max;  /* ms */
    int puma_init;             /* nodo */
    Event *events;
    int n_events, cap_events;
} TestCase;

typedef struct {
    int declared;              /* valor de SUITE_START, 0 si no habia */
    TestCase *cases;
    int n_cases;
} Suite;

/* Carga el archivo. 0 = ok, -1 = error (mensaje legible en err). */
int config_load(const char *path, Suite *suite, char *err, size_t errsz);
void config_free(Suite *suite);
void config_dump(const Suite *suite, FILE *out);

#endif
