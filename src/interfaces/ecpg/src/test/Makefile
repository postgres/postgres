test2: test2.c
	gcc -g -I ../include -I ../../../libpq -o test2 test2.c ../lib/libecpg.a ../../../libpq/libpq.a -lcrypt
test2.c: test2.pgc
	../preproc/ecpg test2.pgc
clean:
	/bin/rm test2 test2.c
