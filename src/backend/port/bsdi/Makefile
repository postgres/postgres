#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/bsdi
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/bsdi/Attic/Makefile,v 1.6 1997/12/20 00:25:19 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

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

