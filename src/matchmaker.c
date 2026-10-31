#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "matchmaker.h"
#include "mesa.h"

/* ---------- estado compartido ---------- */
static atomic_int g_apagado = 0;          /* 1 = hay que cerrar (Ctrl+C, luego) */
static sem_t g_mesas_libres;              /* permisos disponibles = mesas libres */
static pthread_mutex_t g_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;
static int *g_cola;                       /* fila de jugadores esperando pareja */
static int g_q_head = 0, g_q_tail = 0;    /* en la fila: tail - head jugadores */
static int g_llegados = 0, g_total = 0;   /* cuantos clientes ya llegaron / total */
static const TestCase *g_tc;

typedef struct {
    int id_mesa;
    int puma, perros;
} MesaArgs;

static void dormir_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Hilo cliente: llega (tras un retardo) y se anota en la fila. */
static void *hilo_cliente(void *arg) {
    int id = (int)(intptr_t)arg;
    dormir_ms((long)((unsigned)id * 7919u % 100));   /* llegada escalonada */
    pthread_mutex_lock(&g_q_mutex);
    g_cola[g_q_tail++] = id;
    g_llegados++;
    printf("[matchmaker] jugador %d llego\n", id);
    pthread_cond_broadcast(&g_q_cond);               /* avisa al matchmaker */
    pthread_mutex_unlock(&g_q_mutex);
    return NULL;
}

/* Saca 2 jugadores de la fila. Si no hay, duerme (sin busy-wait).
 * Devuelve 1 si saco una pareja, 0 si ya no habra mas parejas. */
static int sacar_pareja(int *puma, int *perros) {
    int ok = 0;
    pthread_mutex_lock(&g_q_mutex);
    while ((g_q_tail - g_q_head) < 2 && g_llegados < g_total)
        pthread_cond_wait(&g_q_cond, &g_q_mutex);
    if ((g_q_tail - g_q_head) >= 2) {
        *puma = g_cola[g_q_head++];
        *perros = g_cola[g_q_head++];
        ok = 1;
    }
    pthread_mutex_unlock(&g_q_mutex);
    return ok;
}

/* Hilo de mesa: juega la partida y devuelve el permiso al semaforo. */
static void *hilo_mesa(void *arg) {
    MesaArgs *a = arg;
    mesa_jugar(a->id_mesa, g_tc, &g_apagado);
    printf("[matchmaker] mesa %d liberada\n", a->id_mesa);
    free(a);
    sem_post(&g_mesas_libres);
    return NULL;
}

int torneo_run(int n, int m, const TestCase *tc) {
    g_tc = tc;
    g_total = n;
    g_cola = calloc((size_t)n, sizeof(int));
    pthread_t *clientes = malloc((size_t)n * sizeof(pthread_t));
    pthread_t *mesas = malloc((size_t)(n / 2 + 1) * sizeof(pthread_t));
    if (!g_cola || !clientes || !mesas) {
        fprintf(stderr, "sin memoria\n");
        free(g_cola); free(clientes); free(mesas);
        return 1;
    }
    sem_init(&g_mesas_libres, 0, (unsigned)m);

    /* 1) lanzar los N clientes */
    for (int i = 0; i < n; i++)
        pthread_create(&clientes[i], NULL, hilo_cliente, (void *)(intptr_t)i);

    /* 2) bucle del matchmaker: pareja -> esperar mesa libre -> lanzar mesa */
    int n_mesas = 0, puma, perros;
    while (sacar_pareja(&puma, &perros)) {
        while (sem_wait(&g_mesas_libres) == -1 && errno == EINTR)
            ;                                        /* espera pasiva */
        MesaArgs *a = malloc(sizeof *a);
        a->id_mesa = n_mesas;
        a->puma = puma;
        a->perros = perros;
        printf("[matchmaker] mesa %d: Puma=jugador %d, Perros=jugador %d\n",
               a->id_mesa, puma, perros);
        pthread_create(&mesas[n_mesas++], NULL, hilo_mesa, a);
    }

    /* 3) esperar a que todos terminen (join) */
    for (int i = 0; i < n; i++) pthread_join(clientes[i], NULL);
    for (int i = 0; i < n_mesas; i++) pthread_join(mesas[i], NULL);

    if (g_q_tail - g_q_head == 1)
        printf("[matchmaker] jugador %d queda sin pareja (N impar)\n", g_cola[g_q_head]);

    /* 4) liberar todo */
    sem_destroy(&g_mesas_libres);
    free(clientes);
    free(mesas);
    free(g_cola);
    g_cola = NULL;
    g_q_head = g_q_tail = g_llegados = 0;
    return 0;
}
