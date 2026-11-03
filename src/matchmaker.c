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

/* ---------- cosas compartidas entre todos los hilos ---------- */

/* Vale 1 cuando alguien apreto Ctrl+C. Todos los hilos lo miran
 * para saber si deben terminar. */
static atomic_int g_apagado = 0;

/* Vale 1 cuando el torneo ya termino. Lo usa el hilo que simula el
 * Ctrl+C del archivo, para no mandarlo si ya no hace falta. */
static atomic_int g_terminado = 0;

/* Semaforo que cuenta las mesas libres. Empieza en M.
 * Cada partida toma una mesa y la devuelve al terminar. */
static sem_t g_mesas_libres;

/* Mutex y condicion que protegen la fila de jugadores esperando. */
static pthread_mutex_t g_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;

/* La fila de jugadores. head es el primero en la fila y tail es donde
 * entra el siguiente. La cantidad esperando es tail - head. */
static int *g_cola;
static int g_q_head = 0, g_q_tail = 0;

/* Cuantos jugadores ya llegaron y cuantos van a llegar en total. */
static int g_llegados = 0, g_total = 0;

/* El caso de prueba que se esta ejecutando (puede ser NULL). */
static const TestCase *g_tc;

/* Datos que se le pasan a cada hilo de mesa. */
typedef struct {
    int id_mesa;
    int puma, perros;
} MesaArgs;

/* Duerme el hilo la cantidad de milisegundos que le pidamos. */
static void dormir_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Cada jugador es un hilo "cliente". Espera un ratito (para que no
 * lleguen todos juntos) y despues se anota en la fila. Si ya se apreto
 * Ctrl+C, no se acepta: el servidor ya esta cerrando. */
static void *hilo_cliente(void *arg) {
    int id = (int)(intptr_t)arg;
    dormir_ms((long)((unsigned)id * 7919u % 100));
    pthread_mutex_lock(&g_q_mutex);          /* solo uno toca la fila a la vez */
    if (!atomic_load(&g_apagado)) {
        g_cola[g_q_tail++] = id;
        printf("[matchmaker] jugador %d llego\n", id);
    } else {
        printf("[matchmaker] jugador %d rechazado (servidor cerrando)\n", id);
    }
    g_llegados++;
    pthread_cond_broadcast(&g_q_cond);       /* avisa al matchmaker que llego alguien */
    pthread_mutex_unlock(&g_q_mutex);
    return NULL;
}

/* Saca dos jugadores de la fila para formar una pareja.
 * Si no hay dos todavia, el hilo se duerme (no gasta CPU) hasta que
 * llegue alguien nuevo o se apriete Ctrl+C.
 * Devuelve 1 si formo una pareja, y 0 si ya no habra mas parejas. */
static int sacar_pareja(int *puma, int *perros) {
    int ok = 0;
    pthread_mutex_lock(&g_q_mutex);
    while (!atomic_load(&g_apagado) &&
           (g_q_tail - g_q_head) < 2 && g_llegados < g_total)
        pthread_cond_wait(&g_q_cond, &g_q_mutex);   /* espera dormido */
    if (!atomic_load(&g_apagado) && (g_q_tail - g_q_head) >= 2) {
        *puma = g_cola[g_q_head++];          /* el primero juega de Puma */
        *perros = g_cola[g_q_head++];        /* el segundo juega de Perros */
        ok = 1;
    }
    pthread_mutex_unlock(&g_q_mutex);
    return ok;
}

/* Cada mesa corre en su propio hilo. Juega la partida completa y, al
 * terminar, devuelve la mesa al semaforo para que otra pareja la use. */
static void *hilo_mesa(void *arg) {
    MesaArgs *a = arg;
    mesa_jugar(a->id_mesa, g_tc, &g_apagado);
    printf("[matchmaker] mesa %d liberada\n", a->id_mesa);
    free(a);
    sem_post(&g_mesas_libres);               /* la mesa queda libre */
    return NULL;
}

/* Este hilo solo espera la senal Ctrl+C. Es el unico que la recibe,
 * porque en los demas hilos la senal esta bloqueada.
 * Cuando llega, avisa a todos y despierta a los que esten dormidos. */
static void *hilo_senales(void *arg) {
    sigset_t *set = arg;
    int sig = 0;
    if (sigwait(set, &sig) != 0) return NULL;
    if (sig == SIGINT) {
        printf("\n[servidor] SIGINT recibido: cerrando ordenadamente...\n");
        atomic_store(&g_apagado, 1);         /* 1) levantamos la bandera */
        pthread_mutex_lock(&g_q_mutex);
        pthread_cond_broadcast(&g_q_cond);   /* 2) despertamos al matchmaker si esperaba jugadores */
        pthread_mutex_unlock(&g_q_mutex);
        sem_post(&g_mesas_libres);           /* 3) lo despertamos tambien si esperaba una mesa */
    }
    return NULL;
}

/* Algunos casos del archivo traen el evento SIGNAL_SIGINT. Este hilo lo
 * simula: espera un rato (para que ya haya partidas jugando) y se manda
 * a si mismo un SIGINT, igual que si el usuario apretara Ctrl+C.
 * Si el torneo termina antes, no manda nada. */
static void *hilo_sigint_programado(void *arg) {
    (void)arg;
    for (int i = 0; i < 30 && !atomic_load(&g_terminado); i++)
        dormir_ms(10);
    if (!atomic_load(&g_terminado))
        kill(getpid(), SIGINT);
    return NULL;
}

/* Funcion principal del torneo: recibe n jugadores y m mesas maximas. */
int torneo_run(int n, int m, const TestCase *tc) {
    g_tc = tc;
    g_total = n;
    atomic_store(&g_apagado, 0);
    atomic_store(&g_terminado, 0);

    /* Pedimos memoria para la fila y para guardar los hilos. */
    g_cola = calloc((size_t)n, sizeof(int));
    pthread_t *clientes = malloc((size_t)n * sizeof(pthread_t));
    pthread_t *mesas = malloc((size_t)(n / 2 + 1) * sizeof(pthread_t));
    if (!g_cola || !clientes || !mesas) {
        fprintf(stderr, "sin memoria\n");
        free(g_cola); free(clientes); free(mesas);
        return 1;
    }

    /* El semaforo parte con m permisos: m mesas libres. */
    sem_init(&g_mesas_libres, 0, (unsigned)m);

    /* 1) Bloqueamos SIGINT antes de crear hilos. Asi todos los hilos
     *    nuevos la heredan bloqueada y solo la recibe el hilo de senales. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t th_sig;
    pthread_create(&th_sig, NULL, hilo_senales, &set);

    /* Vemos si el caso trae SIGNAL_SIGINT. Si lo trae, lanzamos el hilo
     * que simula el Ctrl+C. */
    int hay_sigint = 0;
    pthread_t th_timer;
    if (tc)
        for (int i = 0; i < tc->n_events; i++)
            if (tc->events[i].type == EV_SIGINT) hay_sigint = 1;
    if (hay_sigint)
        pthread_create(&th_timer, NULL, hilo_sigint_programado, NULL);

    /* 2) Lanzamos un hilo por cada jugador. */
    for (int i = 0; i < n; i++)
        pthread_create(&clientes[i], NULL, hilo_cliente, (void *)(intptr_t)i);

    /* 3) Bucle del matchmaker: forma una pareja, espera una mesa libre
     *    y lanza la partida. */
    int n_mesas = 0, puma, perros;
    while (sacar_pareja(&puma, &perros)) {
        /* Espera dormido hasta que haya una mesa libre. El while
         * repite si algo interrumpe la espera. */
        while (sem_wait(&g_mesas_libres) == -1 && errno == EINTR)
            ;
        if (atomic_load(&g_apagado)) {       /* nos despertaron por Ctrl+C */
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

    /* 4) Cierre ordenado: esperamos a que terminen TODOS los hilos
     *    (pthread_join) antes de liberar nada. */
    for (int i = 0; i < n; i++) pthread_join(clientes[i], NULL);
    for (int i = 0; i < n_mesas; i++) pthread_join(mesas[i], NULL);

    atomic_store(&g_terminado, 1);           /* ya no hace falta el Ctrl+C simulado */
    if (hay_sigint) pthread_join(th_timer, NULL);

    /* Contamos quien quedo en la fila sin jugar. */
    pthread_mutex_lock(&g_q_mutex);
    int sobran = g_q_tail - g_q_head;
    if (atomic_load(&g_apagado))
        printf("[matchmaker] %d jugador(es) en espera descartados por el cierre\n", sobran);
    else if (sobran == 1)
        printf("[matchmaker] jugador %d queda sin pareja (N impar)\n", g_cola[g_q_head]);
    pthread_mutex_unlock(&g_q_mutex);

    /* Le mandamos SIGUSR1 al hilo de senales para que deje de esperar.
     * Si ya habia recibido Ctrl+C, no pasa nada. */
    pthread_kill(th_sig, SIGUSR1);
    pthread_join(th_sig, NULL);

    /* 5) Liberamos todo lo que usamos. */
    sem_destroy(&g_mesas_libres);
    free(clientes);
    free(mesas);
    free(g_cola);
    g_cola = NULL;
    g_q_head = g_q_tail = g_llegados = 0;
    return 0;
}
