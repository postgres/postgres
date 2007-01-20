#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gist
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/gist/Makefile,v 1.17 2007/01/20 17:16:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gist
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = gist.o gistutil.o gistxlog.o gistvacuum.o gistget.o gistscan.o \
       gistproc.o gistsplit.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
