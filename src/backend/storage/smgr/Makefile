#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/smgr
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/smgr/Makefile,v 1.11 2000/01/19 02:58:55 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = md.o mm.o smgr.o smgrtype.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

