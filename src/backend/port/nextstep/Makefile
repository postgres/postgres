#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/nextstep
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/nextstep/Makefile,v 1.7 2003/04/24 17:16:13 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/nextstep
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dynloader.o port.o

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
