#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/rtree
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/Makefile,v 1.10 2000/08/31 16:09:43 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/rtree
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = rtget.o rtproc.o rtree.o rtscan.o rtstrat.o

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

