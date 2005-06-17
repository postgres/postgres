#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/transam
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/transam/Makefile,v 1.21 2005/06/17 22:32:42 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/transam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = clog.o transam.o varsup.o xact.o xlog.o xlogutils.o rmgr.o slru.o subtrans.o multixact.o twophase.o twophase_rmgr.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)

# ensure that version checks in xlog.c get recompiled when catversion.h changes
xlog.o: xlog.c $(top_srcdir)/src/include/catalog/catversion.h
