#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/rtree
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/rtree/Makefile,v 1.11 2003/11/29 19:51:40 pgsql Exp $
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

