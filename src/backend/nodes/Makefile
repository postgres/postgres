#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/nodes
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/nodes/Makefile,v 1.12 2000/06/29 02:26:23 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include $(SRCDIR)/Makefile.global

OBJS = nodeFuncs.o nodes.o list.o \
       copyfuncs.o equalfuncs.o makefuncs.o \
       outfuncs.o readfuncs.o print.o read.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
