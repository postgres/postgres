#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/darwin
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/darwin/Makefile,v 1.5 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/darwin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = system.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean:
	rm -f SUBSYS.o $(OBJS)
