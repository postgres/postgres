#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/nextstep
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/nextstep/Makefile,v 1.10 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/nextstep
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = port.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean:
	rm -f SUBSYS.o $(OBJS)
