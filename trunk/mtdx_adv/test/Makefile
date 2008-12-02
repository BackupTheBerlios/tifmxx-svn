CC = gcc
CFLAGS = -I../ -I. -g -fno-inline -D_GNU_SOURCE -DDEBUG

test_ftl: test_ftl.o mtdx_bus.o mtdx_data.o ftl_simple.o rand_peb_alloc.o \
	  long_map.o dummy_kernel.o rbtree.o bitmap.o find_next_bit.o \
	  hweight.o vsprintf.o
	gcc -mthreads -lcrypto -lrt -o $@ $^

mtdx_bus.o: ../mtdx_bus.c
	gcc $(CFLAGS) -c $^

mtdx_data.o: ../mtdx_data.c
	gcc $(CFLAGS) -c $^

ftl_simple.o: ../ftl_simple.c
	gcc $(CFLAGS) -c $^

rand_peb_alloc.o: ../rand_peb_alloc.c
	gcc $(CFLAGS) -c $^

long_map.o: ../long_map.c
	gcc $(CFLAGS) -c $^

clean:
	rm -f *.o test_ftl
