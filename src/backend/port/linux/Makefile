#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/linux
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/linux/Attic/Makefile,v 1.1 1996/10/27 09:50:31 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../include \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

ifdef LINUX_ELF
OBJS = port.o
else
OBJS = dynloader.o
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

