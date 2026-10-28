CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -pthread -g
SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

torneo: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o torneo

.PHONY: clean
