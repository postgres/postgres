#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/common/Makefile,v 1.14 1999/12/09 19:14:25 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS+=-I../..

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = heaptuple.o indextuple.o indexvalid.o printtup.o \
       scankey.o tupdesc.o  

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) -o SUBSYS.o $(OBJS)

heaptuple.o heapvalid.o tupdesc.o: ../../fmgr.h

../../fmgr.h:
	$(MAKE) -C ../.. fmgr.h

dep depend: ../../fmgr.h
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
