#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/nodes
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/nodes/Makefile,v 1.16 2004/01/07 18:43:36 neilc Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/nodes
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = nodeFuncs.o nodes.o list.o bitmapset.o \
       copyfuncs.o equalfuncs.o makefuncs.o \
       outfuncs.o readfuncs.o print.o read.o value.o

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
