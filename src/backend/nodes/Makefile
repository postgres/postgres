#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for nodes
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/nodes/Makefile,v 1.5 1997/12/20 00:24:08 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. 

CFLAGS+=$(INCLUDE_OPT)

OBJS = nodeFuncs.o nodes.o list.o \
       copyfuncs.o equalfuncs.o makefuncs.o outfuncs.o readfuncs.o \
       print.o read.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

