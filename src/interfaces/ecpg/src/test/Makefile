test2: test2.c
	gcc -g -I ../include -I ../../../libpq -o test2 test2.c ../lib/libecpg.a ../../../libpq/libpq.a -lcrypt
test2.c: test2.qc
	../preproc/ecpg < test2.qc > test2.c
