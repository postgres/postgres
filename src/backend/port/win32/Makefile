#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/win32
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/win32/Makefile,v 1.4 2004/04/12 16:19:18 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/win32
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = sema.o shmem.o timer.o socket.o signal.o

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
