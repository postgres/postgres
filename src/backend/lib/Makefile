#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for lib (miscellaneous stuff)
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/lib/Makefile,v 1.20 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/lib
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = dllist.o stringinfo.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
