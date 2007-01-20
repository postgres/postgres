#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/heap
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/heap/Makefile,v 1.14 2007/01/20 17:16:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/heap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heapam.o hio.o tuptoaster.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
