#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/bsdi
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/bsdi/Attic/Makefile,v 1.3 1996/11/01 03:35:54 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../include \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

ifeq ($(PRE_BSDI_2_1), false)
OBJS = port.o
else
OBJS = dynloader.o port.o
endif

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

