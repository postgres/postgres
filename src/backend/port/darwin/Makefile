#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/darwin
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/darwin/Makefile,v 1.3 2002/05/05 01:03:26 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/darwin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = system.o

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
