nanocnp : nanocnp.c nanocnp.h
	gcc -std=gnu11 -o $@ -g -O0 -Wall -Wmissing-prototypes $<
