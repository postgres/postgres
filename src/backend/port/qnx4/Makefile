#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/sparc
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/Makefile,v 1.3 2000/08/31 16:10:19 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/qnx4
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = isnan.o rint.o sem.o shm.o

all: SUBSYS.o tstrint tstsem tstshm

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

tstrint: tstrint.o rint.o
	$(CC) -o tstrint rint.o tstrint.o

tstsem: tstsem.o sem.o
	$(CC) -o tstsem sem.o tstsem.o

tstshm: tstshm.o shm.o
	$(CC) -o tstshm shm.o tstshm.o

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) tstrint tstrint.o tstsem tstsem.o tstshm tstshm.o

ifeq (depend,$(wildcard depend))
include depend
endif
