#
# Makefile, requires pgsql/contrib/pginterface
#
#
PGINTERFACE = ../pginterface/pginterface.o ../pginterface/halt.o # these have to be in your library search path
TARGET = findoidjoins
CFLAGS = -g -Wall -I. -I../pginterface -I../../src/interfaces/libpq  -I/usr/local/pgsql/include
LDFLAGS = -L/usr/local/pgsql/lib -lpq

all : $(TARGET)

findoidjoins:	$(PGINTERFACE) findoidjoins.c
	gcc -o $@ $(CFLAGS) $@.c $(PGINTERFACE) $(LDFLAGS)

clean:
	rm -f *.o $(TARGET) log core

install:
	install -s -o bin -g bin $(TARGET) /usr/local/pgsql/bin

