#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/ipc
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/ipc/Makefile,v 1.8 1999/07/22 18:30:07 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

# seems to be required 1999/07/22 bjm
ifeq ($(CPU),alpha)
ifeq ($(CC), gcc)
CFLAGS+= -fno-inline
endif
ifeq ($(CC), egcs)
CFLAGS+= -fno-inline
endif
endif

OBJS = ipc.o ipci.o shmem.o shmqueue.o sinval.o \
       sinvaladt.o spin.o

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

