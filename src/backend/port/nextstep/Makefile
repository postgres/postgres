#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/nextstep
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/nextstep/Makefile,v 1.9 2006/07/15 03:33:14 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/nextstep
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = port.o

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
