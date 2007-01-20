#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/nodes
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/nodes/Makefile,v 1.19 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/nodes
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = nodeFuncs.o nodes.o list.o bitmapset.o tidbitmap.o \
       copyfuncs.o equalfuncs.o makefuncs.o \
       outfuncs.o readfuncs.o print.o read.o params.o value.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
