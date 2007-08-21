#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/cache
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/cache/Makefile,v 1.22 2007/08/21 01:11:19 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = catcache.o inval.o plancache.o relcache.o \
	syscache.o lsyscache.o typcache.o ts_cache.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
