all: test1 test2 test3 test4 perftest dyntest dyntest2 test_notice

#LDFLAGS=-g -I /usr/local/pgsql/include -L/usr/local/pgsql/lib -lecpg -lpq
LDFLAGS=-g -I ../include -I /usr/include/postgresql -L /usr/lib -lecpg -lpq

#ECPG=/usr/local/pgsql/bin/ecpg -I../include
ECPG=../preproc/ecpg -I../include

.SUFFIXES: .pgc .c

test1: test1.c
test2: test2.c
test3: test3.c
test4: test4.c
perftest: perftest.c
dyntest: dyntest.c
dyntest2: dyntest2.c
test_code100: test_code100.c
test_notice: test_notice.c
test_init: test_init.c

.pgc.c:
	$(ECPG) $? 

clean:
	rm -f test1 test2 test3 test4 perftest *.c log dyntest dyntest2
