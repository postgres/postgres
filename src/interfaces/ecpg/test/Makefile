all: test1 test2 test3 test4 perftest dyntest dyntest2

LDFLAGS=-g -I /usr/local/pgsql/include -L/usr/local/pgsql/lib -lecpg -lpq

ECPG=../preproc/ecpg -I../include

.SUFFIXES: .pgc .c

test1: test1.c
test2: test2.c
test3: test3.c
test4: test4.c
perftest: perftest.c
dyntest: dyntest.c
dyntest2: dyntest2.c

.pgc.c:
	$(ECPG) $? 

clean:
	rm -f test1 test2 test3 test4 perftest *.c *.o log dyntest dyntest2
