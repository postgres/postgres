#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/port/win32
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/win32/Makefile,v 1.7 2006/04/29 20:52:56 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/win32
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = shmem.o timer.o socket.o signal.o security.o error.o

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
