#
# Makefile
#
#
PGINTERFACE = pginterface.o halt.o 
TARGET = pginsert pgwordcount pgnulltest
CFLAGS = -g -fpic -Wall -I. -I../../src/interfaces/libpq  -I/usr/local/pgsql/include
LDFLAGS = -L/usr/local/pgsql/lib -lpq

all : $(TARGET)

pginsert:	$(PGINTERFACE) pginsert.c
	gcc -o $@ $(CFLAGS) $@.c $(PGINTERFACE) $(LDFLAGS)

pgwordcount:	$(PGINTERFACE) pgwordcount.c
	gcc -o $@ $(CFLAGS) $@.c $(PGINTERFACE) $(LDFLAGS)

pgnulltest:	$(PGINTERFACE) pgnulltest.c
	gcc -o $@ $(CFLAGS) $@.c $(PGINTERFACE) $(LDFLAGS)

pginterface.o:	pginterface.c
	gcc -c $(CFLAGS) pginterface.c

halt.o:	halt.c
	gcc -c $(CFLAGS) halt.c

clean:
	rm -f *.o $(TARGET) log core

install:
	install -s -o bin -g bin $(TARGET) /usr/local/pgsql/bin

