all: test1 test2 test3 test4 perftest dyntest dyntest2

LDFLAGS=-g -I /usr/local/pgsql/include -L/usr/local/pgsql/lib -lecpg -lpq -lcrypt
#LDFLAGS=-g -I../include -I/usr/include/postgresql  -L../lib -L/usr/lib/postgresql -lecpg -lpq -lcrypt 
#LDFLAGS=-g -I/usr/include/postgresql -lecpg -lpq -lcrypt

#ECPG=/usr/local/pgsql/bin/ecpg
ECPG=../preproc/ecpg -I../include
#ECPG=/usr/bin/ecpg -I/usr/include/postgresql

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
	-/bin/rm test1 test2 test3 test4 perftest *.c log dyntest dyntest2
