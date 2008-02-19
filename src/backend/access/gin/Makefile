#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gin
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/gin/Makefile,v 1.3 2008/02/19 10:30:06 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = ginutil.o gininsert.o ginxlog.o ginentrypage.o gindatapage.o \
	ginbtree.o ginscan.o ginget.o ginvacuum.o ginarrayproc.o \
	ginbulk.o

include $(top_srcdir)/src/backend/common.mk
