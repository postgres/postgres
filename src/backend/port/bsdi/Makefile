#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/bsdi
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/bsdi/Attic/Makefile,v 1.5 1997/03/19 03:01:25 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = dynloader.o 

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

