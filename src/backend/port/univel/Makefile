#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/univel
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/univel/Attic/Makefile,v 1.2 1997/12/19 02:56:35 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../../include

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

