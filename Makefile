CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -pthread -g
SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
LIB_OBJ = $(filter-out src/main.o,$(OBJ))

torneo: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

test_board: pruebas/test_board.c $(LIB_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

test: test_board
	./test_board

clean:
	rm -f src/*.o torneo test_board

.PHONY: clean test
