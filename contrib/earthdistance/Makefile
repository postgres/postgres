# PGLIB is probably /usr/local/pgsql/lib

PGINCLUDE=${PGLIB}/../include
CFLAGS+=-I${PGINCLUDE}

install-earthdistance: ${PGLIB}/earthdistance.so

${PGLIB}/earthdistance.so: earthdistance.so
	sudo install -C -g bin -o bin earthdistance.so ${PGLIB}

earthdistance.so:  earthdistance.o
	$(LD) -o $@ -Bshareable $<

earthdistance.o:  earthdistance.c
	$(CC) -o $@ -c $(CFLAGS) $<
