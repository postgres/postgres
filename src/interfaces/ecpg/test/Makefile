all: test1 test2 perftest

LDFLAGS=-g -I ../include -I ../../libpq -L../lib -lecpg -L../../libpq -lpq -lcrypt --static

test1: test1.c
test1.c: test1.pgc
	../preproc/ecpg $?

test2: test2.c
test2.c: test2.pgc
	../preproc/ecpg $?

perftest: perftest.c
perftest.c:perftest.pgc
	../preproc/ecpg $?

clean:
	/bin/rm test1 test2 perftest *.c log
