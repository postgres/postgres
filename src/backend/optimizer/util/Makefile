#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/util
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/util/Makefile,v 1.6 1998/04/06 00:23:55 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = clauseinfo.o clauses.o indexnode.o internal.o plancat.o \
       joininfo.o keys.o ordering.o pathnode.o relnode.o tlist.o var.o

# not ready yet: predmig.o xfunc.o

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

