all: test2 perftest

test2: test2.c
	gcc -g -I ../include -I ../../../libpq -o test2 test2.c ../lib/libecpg.a ../../../libpq/libpq.a -lcrypt
test2.c: test2.pgc
	../preproc/ecpg test2.pgc

perftest: perftest.c
	gcc -g -I ../include -I ../../../libpq -o perftest perftest.c ../lib/libecpg.a ../../../libpq/libpq.a -lcrypt
perftest.c: perftest.pgc
	../preproc/ecpg perftest.pgc

clean:
	/bin/rm test2 test2.c perftest perftest.c
