#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/error
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/error/Makefile,v 1.5 1998/04/06 00:26:45 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = assert.o elog.o exc.o excabort.o excid.o format.o

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

