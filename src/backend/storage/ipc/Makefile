#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/ipc
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/ipc/Makefile,v 1.1 1996/10/27 09:52:10 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
              -I../../include \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = ipc.o ipci.o s_lock.o shmem.o shmqueue.o sinval.o \
       sinvaladt.o spin.o

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

