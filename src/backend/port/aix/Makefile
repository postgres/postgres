#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/aix
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/aix/Attic/Makefile,v 1.2 1996/11/09 06:18:48 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = dlfcn.o

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

