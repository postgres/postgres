#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/lmgr
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/lmgr/Makefile,v 1.8 1998/07/26 04:30:40 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..
ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = lmgr.o lock.o multi.o proc.o single.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

