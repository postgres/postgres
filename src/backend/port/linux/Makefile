#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/linux
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/linux/Attic/Makefile,v 1.2 1996/11/09 06:19:51 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
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

