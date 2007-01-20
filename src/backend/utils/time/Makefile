#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/time
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/time/Makefile,v 1.11 2007/01/20 17:16:15 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/time
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = tqual.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
