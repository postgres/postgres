#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/hash
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/hash/Makefile,v 1.13 2007/01/20 17:16:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/hash
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = hash.o hashfunc.o hashinsert.o hashovfl.o hashpage.o hashscan.o \
       hashsearch.o hashutil.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
