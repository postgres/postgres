#
# Makefile for storage/ipc
#
# $Header: /cvsroot/pgsql/src/backend/storage/ipc/Makefile,v 1.13 2000/08/31 16:10:33 petere Exp $
#

subdir = src/backend/storage/ipc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# seems to be required 1999/07/22 bjm
ifeq ($(CPU), alpha)
ifeq ($(GCC), yes)
CFLAGS+= -fno-inline
endif
endif

OBJS = ipc.o ipci.o shmem.o shmqueue.o sinval.o \
       sinvaladt.o spin.o

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
