#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/hash
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/hash/Makefile,v 1.11 2003/11/09 21:30:35 tgl Exp $
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

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

