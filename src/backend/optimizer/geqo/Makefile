#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the genetic query optimizer module
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/backend/optimizer/geqo/Makefile,v 1.18 2003/11/29 19:51:50 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/geqo
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS =	geqo_copy.o geqo_eval.o geqo_main.o geqo_misc.o \
	geqo_mutation.o geqo_pool.o geqo_recombination.o \
	geqo_selection.o \
	geqo_erx.o geqo_pmx.o geqo_cx.o geqo_px.o geqo_ox1.o geqo_ox2.o

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
