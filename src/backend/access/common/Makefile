#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/common/Makefile,v 1.12 1998/07/26 04:30:16 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS+=-I../..

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = heaptuple.o heapvalid.o indextuple.o indexvalid.o printtup.o \
       scankey.o tupdesc.o  

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

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
