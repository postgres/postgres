#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/linux
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/linuxalpha/Attic/Makefile,v 1.2 1997/12/20 00:26:08 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

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
