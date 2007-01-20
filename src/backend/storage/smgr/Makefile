#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/smgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/smgr/Makefile,v 1.16 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/smgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = md.o smgr.o smgrtype.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
