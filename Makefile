CC     = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2

mysh: mysh.c
	$(CC) $(CFLAGS) -o mysh mysh.c

clean:
	rm -f mysh

.PHONY: clean
