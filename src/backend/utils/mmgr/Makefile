#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mmgr
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mmgr/Makefile,v 1.5 1998/04/06 00:27:24 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = aset.o mcxt.o palloc.o portalmem.o oset.o

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

