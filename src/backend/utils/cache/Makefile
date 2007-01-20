#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/cache
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/cache/Makefile,v 1.20 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = catcache.o inval.o relcache.o syscache.o lsyscache.o typcache.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
