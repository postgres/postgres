#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/common/Makefile,v 1.8 1997/12/20 00:22:11 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

CFLAGS+=$(INCLUDE_OPT) 

OBJS = heaptuple.o heapvalid.o indextuple.o indexvalid.o printtup.o \
       scankey.o tupdesc.o  

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

heapvalid.o tupdesc.o: ../../fmgr.h

../../fmgr.h:
	$(MAKE) -C ../.. fmgr.h

dep: ../../fmgr.h
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

