#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/svr4
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/svr4/Attic/Makefile,v 1.2 1996/11/09 06:20:26 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = port.o

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

