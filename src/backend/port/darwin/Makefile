#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/darwin
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/darwin/Makefile,v 1.1 2000/12/11 00:49:54 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/darwin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = sem.o

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
