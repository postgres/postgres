#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/plan
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/optimizer/plan/Makefile,v 1.14 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/plan
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = createplan.o initsplan.o planagg.o planmain.o planner.o \
	setrefs.o subselect.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
