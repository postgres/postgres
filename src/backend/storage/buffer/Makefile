#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/buffer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/buffer/Makefile,v 1.8 1998/04/29 12:37:51 scrappy Exp $
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
	rm -f SUBSYS.o $(OBJS) tas_test

tas_test: s_lock.c
	$(CC) $(CFLAGS) -DTAS_TEST=1 -g s_lock.c -o tas_test
	./tas_test

ifeq (depend,$(wildcard depend))
include depend
endif
