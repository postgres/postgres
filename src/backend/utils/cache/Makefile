#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/cache
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/cache/Makefile,v 1.17.2.1 2003/09/07 04:36:55 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = catcache.o inval.o relcache.o syscache.o lsyscache.o typcache.o

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
