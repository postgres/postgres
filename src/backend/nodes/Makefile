#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for nodes
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/nodes/Makefile,v 1.9 1999/12/09 19:14:42 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

OBJS = nodeFuncs.o nodes.o list.o \
       copyfuncs.o equalfuncs.o freefuncs.o makefuncs.o outfuncs.o \
       readfuncs.o print.o read.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) -o SUBSYS.o $(OBJS)

outfuncs.o: ../parse.h

../parse.h:
	$(MAKE) -C .. parse.h

depend dep: ../parse.h
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

