#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/file
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/file/Makefile,v 1.5 1998/04/06 00:25:05 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = fd.o

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

