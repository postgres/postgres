#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for pgeasy examples
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgeasy/examples/Attic/Makefile,v 1.2 2000/05/18 14:24:37 momjian Exp $
#
#-------------------------------------------------------------------------

CFLAGS=-I/usr/local/pgsql/include
TARGET = pginsert pgwordcount pgnulltest
LDFLAGS = -L/usr/local/pgsql/lib -lpgeasy

all : $(TARGET)

pginsert:
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

pgwordcount:
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

pgnulltest:
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

clean:
	rm -f *.o $(TARGET) log core

