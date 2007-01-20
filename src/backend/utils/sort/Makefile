#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/sort
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/sort/Makefile,v 1.14 2007/01/20 17:16:14 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/sort
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = logtape.o tuplesort.o tuplestore.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
