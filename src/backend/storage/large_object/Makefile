#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/large_object
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/large_object/Makefile,v 1.6 1998/04/06 00:25:30 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = inv_api.o

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

