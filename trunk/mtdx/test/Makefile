CC = gcc
CFLAGS = -I../ -I. -g

test_ftl: test_ftl.o ftl_simple.o rand_peb_alloc.o long_map.o dummy_kernel.o \
	  rbtree.o bitmap.o find_next_bit.o sg_bounce.o hweight.o vsprintf.o
	gcc -lcrypto -lrt -o $@ $^

ftl_simple.o: ../ftl_simple.c
	gcc $(CFLAGS) -c $^

rand_peb_alloc.o: ../rand_peb_alloc.c
	gcc $(CFLAGS) -c $^

long_map.o: ../long_map.c
	gcc $(CFLAGS) -c $^

sg_bounce.o: ../sg_bounce.c
	gcc $(CFLAGS) -c $^

