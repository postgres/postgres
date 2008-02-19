#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/transam
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/transam/Makefile,v 1.22 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/transam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = clog.o transam.o varsup.o xact.o xlog.o xlogutils.o rmgr.o slru.o subtrans.o multixact.o twophase.o twophase_rmgr.o

include $(top_srcdir)/src/backend/common.mk

# ensure that version checks in xlog.c get recompiled when catversion.h changes
xlog.o: xlog.c $(top_srcdir)/src/include/catalog/catversion.h
