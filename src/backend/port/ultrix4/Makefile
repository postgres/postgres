#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/ultrix
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/ultrix4/Attic/Makefile,v 1.1 1996/10/27 09:51:05 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../include \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = dynloader.o port.o strdup.o

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

