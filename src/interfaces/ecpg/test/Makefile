all: test2 perftest

test2: test2.c
	gcc -g -I ../include -I ../../libpq -o test2 test2.c -L../lib -lecpg -L../../libpq -lpq -lcrypt --static
test2.c: test2.pgc
	../preproc/ecpg test2.pgc

perftest: perftest.c
	gcc -g -I ../include -I ../../libpq -o perftest perftest.c -L../lib -lecpg -L../../libpq -lpq -lcrypt --static
perftest.c: perftest.pgc
	../preproc/ecpg perftest.pgc

clean:
	/bin/rm test2 test2.c perftest perftest.c log
