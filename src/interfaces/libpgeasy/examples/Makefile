#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for pgeasy examples
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgeasy/examples/Attic/Makefile,v 1.3 2002/03/04 18:50:21 momjian Exp $
#
#-------------------------------------------------------------------------

CFLAGS=-I/usr/local/pgsql/include
TARGET = pginsert pgwordcount pgnulltest pgmultiresult
LDFLAGS = -L/usr/local/pgsql/lib -lpgeasy

all : $(TARGET)

%: %.c
	gcc -o $@ $(CFLAGS) $@.c $(PGEASY) $(LDFLAGS)

clean:
	rm -f *.o $(TARGET) log core

