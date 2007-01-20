#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/resowner
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/resowner/Makefile,v 1.2 2007/01/20 17:16:14 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/resowner
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = resowner.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
