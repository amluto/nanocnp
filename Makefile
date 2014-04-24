all : nanocnp

nanocnp.o : nanocnp.c nanocnp.h
	gcc -std=gnu11 -o $@ -c -g -O2 -Wall -Wmissing-prototypes $<

main.o : main.c nanocnp.h
	gcc -std=gnu11 -o $@ -c -g -O2 -Wall -Wmissing-prototypes $<

nanocnp : nanocnp.o main.o
	gcc -std=gnu11 -o $@ -g -O2 -Wall -Wmissing-prototypes $^
