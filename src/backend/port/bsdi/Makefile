#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/bsdi
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/bsdi/Attic/Makefile,v 1.4 1996/11/09 06:19:06 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
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

