#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/init
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/init/Makefile,v 1.21 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/init
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = flatfiles.o globals.o miscinit.o postinit.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
