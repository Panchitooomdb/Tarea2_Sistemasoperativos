# Torneo de Komikan Concurrente

Tarea 2 - Sistemas Operativos. Autores: Alexis (AlexisKGB) y Francisco Grandon (Panchitooomdb).

Programa en C con hilos (pthreads). Un "matchmaker" junta jugadores de a 2 (uno es el Puma y el otro los Perros) y los pone en una mesa. Nunca hay mas de M mesas jugando a la vez.

## Como compilar y ejecutar

    make                 # compila
    make test            # prueba el tablero
    make clean           # borra lo compilado

    ./torneo N M [config.txt] [caso]

- N: cuantos jugadores llegan.
- M: maximo de mesas al mismo tiempo.
- config.txt: archivo de casos (opcional), por ejemplo pruebas/test_cases.txt.
- caso: numero de caso (opcional). Si lo pones, corre solo ese.

Ejemplos:

    ./torneo 6 2
    ./torneo 2 1 pruebas/test_cases.txt
    ./torneo 2 1 pruebas/test_cases.txt 5

Si un caso trae SPAWN_PLAYERS, se usan su COUNT y MESAS_MAX como N y M.

## Archivo de configuracion

Cada caso tiene una linea CONFIG y varias lineas EVENT:

    TEST_CASE 1 "Nombre"
    CONFIG SEED 42 DELAY_MIN 50 DELAY_MAX 300 PUMA_INIT 2 2
    EVENT MOVE PUMA 0 FROM 2 2 TO 2 3
    TEST_END 1

- SEED: semilla del azar (para repetir pruebas).
- DELAY_MIN / DELAY_MAX: tiempo de espera entre movimientos (ms).
- PUMA_INIT: donde parte el Puma (fila columna).

## Como sincronizamos (resumen)

- Tablero: un mutex. Se revisa y se hace el movimiento dentro del mismo candado, asi solo una ficha gana cada casilla.
- Mesa: un hilo por ficha y un hilo Juez. Las fichas dejan avisos en una fila y solo el Juez imprime.
- Fin de partida: un atomic_exchange hace que solo el primer evento decida como termino.
- Matchmaker: un semaforo que parte en M controla las mesas. Si no hay mesa libre, espera dormido.
- Desconexion: si un jugador se cae, gana el otro por abandono.
- Ctrl+C (SIGINT): un solo hilo la recibe con sigwait y avisa a todos para cerrar ordenado. Valgrind no muestra fugas.

## Informe

El informe (2 paginas) va en la carpeta informe/.
