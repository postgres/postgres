#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for pgeasy examples
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgeasy/examples/Attic/Makefile,v 1.1 1999/10/11 18:03:01 momjian Exp $
#
#-------------------------------------------------------------------------

TARGET = pginsert pgwordcount pgnulltest
LDFLAGS = -lpgeasy

all : $(TARGET)

pginsert:
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

pgwordcount:
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

pgnulltest:
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

clean:
	rm -f *.o $(TARGET) log core

