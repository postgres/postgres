all: stp.so test1 test2 test3 test4 test5 perftest dyntest

#LDFLAGS=-g -I /usr/local/pgsql/include -L/usr/local/pgsql/lib -lecpg -lpq -lcrypt
LDFLAGS=-g -I../include -I/usr/include/postgresql  -L/usr/lib/postgresql -L../lib -lecpg -lpq -lcrypt
#LDFLAGS=-g -I/usr/include/postgresql -lecpg -lpq -lcrypt

#ECPG=/usr/local/pgsql/bin/ecpg
ECPG=../preproc/ecpg -I../include
#ECPG=/usr/bin/ecpg -I/usr/include/postgresql

.SUFFIXES: .pgc .c

test1: test1.c
test2: test2.c
test3: test3.c
test4: test4.c
test5: test5.c
perftest: perftest.c
dyntest: dyntest.c

.pgc.c:
	$(ECPG) $? 

stp.so: stp.c
	cc -fPIC -I../include -I/usr/include/postgresql -c -o stp.o stp.c
	ld -Bdynamic -shared -soname stp.so -o stp.so stp.o -lpq -lecpg -lc


clean:
	-/bin/rm test1 test2 test3 test4 test5 perftest *.c log stp.o stp.so dyntest
