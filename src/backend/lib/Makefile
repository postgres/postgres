#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for lib (miscellaneous stuff)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/lib/Makefile,v 1.1 1996/10/27 09:47:37 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../include \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = bit.o fstack.o hasht.o lispsort.o qsort.o stringinfo.o dllist.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

