#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/transam
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/transam/Makefile,v 1.14 2001/08/24 14:07:48 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/transam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = transam.o transsup.o varsup.o xact.o xid.o xlog.o xlogutils.o rmgr.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)

# ensure that version checks in xlog.c get recompiled when catversion.h changes
xlog.o: xlog.c $(top_srcdir)/src/include/catalog/catversion.h
