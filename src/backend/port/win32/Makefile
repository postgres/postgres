#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/win32
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/win32/Makefile,v 1.1 2003/04/24 21:25:34 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/win32
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = sema.o shmem.o

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
