#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for nodes
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/nodes/Makefile,v 1.6 1998/04/06 00:23:00 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

OBJS = nodeFuncs.o nodes.o list.o \
       copyfuncs.o equalfuncs.o makefuncs.o outfuncs.o readfuncs.o \
       print.o read.o

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

