#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/mmgr/Makefile,v 1.12 2007/01/20 17:16:14 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = aset.o mcxt.o portalmem.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
