all: test1 test2 test3 test4 perftest

LDFLAGS=-g -I /usr/local/pgsql/include -L/usr/local/pgsql/lib -lecpg -lpq -lcrypt

.SUFFIXES: .pgc .c

test1: test1.c
test2: test2.c
test3: test3.c
test4: test4.c
perftest: perftest.c

.pgc.c:
	/usr/local/pgsql/bin/ecpg $?

clean:
	-/bin/rm test1 test2 test3 test4 perftest *.c log
