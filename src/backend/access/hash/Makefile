#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/hash
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/hash/Makefile,v 1.4 1997/12/17 04:30:52 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

ifdef PORTNAME
  INCLUDE_OPT += -I../../port/$(PORTNAME) 
endif

CFLAGS += $(INCLUDE_OPT)

OBJS = hash.o hashfunc.o hashinsert.o hashovfl.o hashpage.o hashscan.o \
       hashsearch.o hashstrat.o hashutil.o

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

