#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "matchmaker.h"
#include "mesa.h"

/* ---------- estado compartido ---------- */
static atomic_int g_apagado = 0;          /* 1 = Ctrl+C recibido, hay que cerrar */
static atomic_int g_terminado = 0;        /* 1 = el torneo ya acabo */
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

/* Hilo cliente: llega (tras un retardo) y se anota en la fila.
 * Si ya se pidio apagar, el jugador es rechazado (no se aceptan nuevos). */
static void *hilo_cliente(void *arg) {
    int id = (int)(intptr_t)arg;
    dormir_ms((long)((unsigned)id * 7919u % 100));   /* llegada escalonada */
    pthread_mutex_lock(&g_q_mutex);
    if (!atomic_load(&g_apagado)) {
        g_cola[g_q_tail++] = id;
        printf("[matchmaker] jugador %d llego\n", id);
    } else {
        printf("[matchmaker] jugador %d rechazado (servidor cerrando)\n", id);
    }
    g_llegados++;
    pthread_cond_broadcast(&g_q_cond);               /* avisa al matchmaker */
    pthread_mutex_unlock(&g_q_mutex);
    return NULL;
}

/* Saca 2 jugadores de la fila. Si no hay, duerme (sin busy-wait).
 * Devuelve 1 si saco una pareja, 0 si ya no habra mas (fin o apagado). */
static int sacar_pareja(int *puma, int *perros) {
    int ok = 0;
    pthread_mutex_lock(&g_q_mutex);
    while (!atomic_load(&g_apagado) &&
           (g_q_tail - g_q_head) < 2 && g_llegados < g_total)
        pthread_cond_wait(&g_q_cond, &g_q_mutex);
    if (!atomic_load(&g_apagado) && (g_q_tail - g_q_head) >= 2) {
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

/* Hilo de senales: espera SIGINT con sigwait. Al llegar, pide el apagado
 * y despierta a todo el que pueda estar durmiendo. */
static void *hilo_senales(void *arg) {
    sigset_t *set = arg;
    int sig = 0;
    if (sigwait(set, &sig) != 0) return NULL;
    if (sig == SIGINT) {
        printf("\n[servidor] SIGINT recibido: cerrando ordenadamente...\n");
        atomic_store(&g_apagado, 1);                 /* 1) levantar el flag */
        pthread_mutex_lock(&g_q_mutex);
        pthread_cond_broadcast(&g_q_cond);           /* 2) despierta cond_wait */
        pthread_mutex_unlock(&g_q_mutex);
        sem_post(&g_mesas_libres);                   /* 3) despierta sem_wait */
    }
    return NULL;
}

/* Simula el evento SIGNAL_SIGINT del archivo de casos: espera un momento
 * (para que haya partidas en curso) y se manda SIGINT al propio proceso,
 * igual que un Ctrl+C del usuario. Si el torneo termina antes, no lo envia. */
static void *hilo_sigint_programado(void *arg) {
    (void)arg;
    for (int i = 0; i < 30 && !atomic_load(&g_terminado); i++)
        dormir_ms(10);
    if (!atomic_load(&g_terminado))
        kill(getpid(), SIGINT);
    return NULL;
}

int torneo_run(int n, int m, const TestCase *tc) {
    g_tc = tc;
    g_total = n;
    atomic_store(&g_apagado, 0);
    atomic_store(&g_terminado, 0);
    g_cola = calloc((size_t)n, sizeof(int));
    pthread_t *clientes = malloc((size_t)n * sizeof(pthread_t));
    pthread_t *mesas = malloc((size_t)(n / 2 + 1) * sizeof(pthread_t));
    if (!g_cola || !clientes || !mesas) {
        fprintf(stderr, "sin memoria\n");
        free(g_cola); free(clientes); free(mesas);
        return 1;
    }
    sem_init(&g_mesas_libres, 0, (unsigned)m);

    /* 1) bloquear SIGINT y SIGUSR1 ANTES de crear hilos (los hijos heredan
     *    la mascara). Solo el hilo de senales las recibe, con sigwait. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t th_sig;
    pthread_create(&th_sig, NULL, hilo_senales, &set);

    /* si el caso trae SIGNAL_SIGINT, lo disparamos nosotros */
    int hay_sigint = 0;
    pthread_t th_timer;
    if (tc)
        for (int i = 0; i < tc->n_events; i++)
            if (tc->events[i].type == EV_SIGINT) hay_sigint = 1;
    if (hay_sigint)
        pthread_create(&th_timer, NULL, hilo_sigint_programado, NULL);

    /* 2) lanzar los N clientes */
    for (int i = 0; i < n; i++)
        pthread_create(&clientes[i], NULL, hilo_cliente, (void *)(intptr_t)i);

    /* 3) bucle del matchmaker: pareja -> esperar mesa libre -> lanzar mesa */
    int n_mesas = 0, puma, perros;
    while (sacar_pareja(&puma, &perros)) {
        while (sem_wait(&g_mesas_libres) == -1 && errno == EINTR)
            ;                                        /* espera pasiva */
        if (atomic_load(&g_apagado)) {               /* nos desperto Ctrl+C */
            printf("[matchmaker] apagado: pareja (%d,%d) no alcanzo mesa\n",
                   puma, perros);
            break;
        }
        MesaArgs *a = malloc(sizeof *a);
        a->id_mesa = n_mesas;
        a->puma = puma;
        a->perros = perros;
        printf("[matchmaker] mesa %d: Puma=jugador %d, Perros=jugador %d\n",
               a->id_mesa, puma, perros);
        pthread_create(&mesas[n_mesas++], NULL, hilo_mesa, a);
    }

    /* 4) cierre ordenado: esperar a TODOS los hilos */
    for (int i = 0; i < n; i++) pthread_join(clientes[i], NULL);
    for (int i = 0; i < n_mesas; i++) pthread_join(mesas[i], NULL);

    atomic_store(&g_terminado, 1);
    if (hay_sigint) pthread_join(th_timer, NULL);

    pthread_mutex_lock(&g_q_mutex);
    int sobran = g_q_tail - g_q_head;
    if (atomic_load(&g_apagado))
        printf("[matchmaker] %d jugador(es) en espera descartados por el cierre\n", sobran);
    else if (sobran == 1)
        printf("[matchmaker] jugador %d queda sin pareja (N impar)\n", g_cola[g_q_head]);
    pthread_mutex_unlock(&g_q_mutex);

    pthread_kill(th_sig, SIGUSR1);   /* despierta sigwait si no hubo Ctrl+C */
    pthread_join(th_sig, NULL);

    /* 5) liberar todo */
    sem_destroy(&g_mesas_libres);
    free(clientes);
    free(mesas);
    free(g_cola);
    g_cola = NULL;
    g_q_head = g_q_tail = g_llegados = 0;
    return 0;
}
