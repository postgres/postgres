#
# Makefile for storage/ipc
#
# $Header: /cvsroot/pgsql/src/backend/storage/ipc/Makefile,v 1.17 2001/11/04 19:55:31 tgl Exp $
#

subdir = src/backend/storage/ipc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# seems to be required 1999/07/22 bjm
ifeq "$(findstring alpha,$(host_cpu))" "alpha"
ifeq "$(GCC)" "yes"
override CFLAGS+= -fno-inline
endif
endif

OBJS = ipc.o ipci.o pmsignal.o shmem.o shmqueue.o sinval.o sinvaladt.o

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
