#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/univel
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/univel/Attic/Makefile,v 1.3 1997/12/20 00:26:45 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

CFLAGS+=$(INCLUDE_OPT)

OBJS = #tas.o

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

