#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/buffer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/buffer/Makefile,v 1.7 1998/04/06 00:24:58 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = buf_table.o buf_init.o bufmgr.o freelist.o localbuf.o s_lock.o

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
