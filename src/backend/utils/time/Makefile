#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/time
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/time/Makefile,v 1.5 1998/04/06 00:27:42 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = tqual.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

