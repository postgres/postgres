#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/heap
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/heap/Makefile,v 1.15 2007/04/08 01:26:27 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/heap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heapam.o hio.o rewriteheap.o tuptoaster.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
