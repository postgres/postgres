#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for lib (miscellaneous stuff)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/lib/Makefile,v 1.14 2000/06/28 03:31:34 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

OBJS = bit.o hasht.o lispsort.o stringinfo.o dllist.o

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

