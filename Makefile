all : nanocnp

.PHONY : clean
clean :
	rm -f nanocnp.o main.o nanocnp

CC := gcc

nanocnp.o : nanocnp.c nanocnp.h
	$(CC) -std=gnu11 -o $@ -c -g -O2 -Wall -Wmissing-prototypes $<

main.o : main.c nanocnp.h
	$(CC) -std=gnu11 -o $@ -c -g -O2 -Wall -Wmissing-prototypes $<

nanocnp : nanocnp.o main.o
	$(CC) -std=gnu11 -o $@ -g -O2 -Wall -Wmissing-prototypes $^
