#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/qnx4
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/qnx4/Makefile,v 1.6 2003/11/29 19:51:54 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/qnx4
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = isnan.o sem.o shm.o

all: SUBSYS.o tstsem tstshm

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

tstsem: tstsem.o sem.o
	$(CC) -o tstsem sem.o tstsem.o

tstshm: tstshm.o shm.o
	$(CC) -o tstshm shm.o tstshm.o

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) tstsem tstsem.o tstshm tstshm.o

ifeq (depend,$(wildcard depend))
include depend
endif
