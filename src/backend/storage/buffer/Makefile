#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/buffer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/buffer/Makefile,v 1.9 1998/05/04 15:44:39 scrappy Exp $
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
	rm -f SUBSYS.o $(OBJS) s_lock_test

s_lock_test: s_lock.c
	$(CC) $(CFLAGS) -DS_LOCK_TEST=1 -g s_lock.c -o s_lock_test
	./s_lock_test

ifeq (depend,$(wildcard depend))
include depend
endif
