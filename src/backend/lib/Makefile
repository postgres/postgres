#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for lib (miscellaneous stuff)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/lib/Makefile,v 1.16 2001/10/05 17:28:12 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/lib
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = bit.o dllist.o lispsort.o stringinfo.o

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
