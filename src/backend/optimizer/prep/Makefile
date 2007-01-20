#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/prep
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/optimizer/prep/Makefile,v 1.16 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/prep
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = prepjointree.o prepqual.o preptlist.o prepunion.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
