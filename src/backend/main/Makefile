#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for main
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/main/Makefile,v 1.11 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/main
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = main.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
