all: test1 test2 perftest

LDFLAGS=-g -I /usr/local/pgsql/include -L/usr/local/pgsql/lib -lecpg -lpq -lcrypt

test1: test1.c
test1.c: test1.pgc
	/usr/local/pgsql/bin/ecpg $?

test2: test2.c
test2.c: test2.pgc
	/usr/local/pgsql/bin/ecpg $?

perftest: perftest.c
perftest.c:perftest.pgc
	/usr/local/pgsql/bin/ecpg $?

clean:
	-/bin/rm test1 test2 perftest *.c log
