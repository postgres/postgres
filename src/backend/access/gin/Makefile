#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gin
#
# IDENTIFICATION
#    src/backend/access/gin/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = ginutil.o gininsert.o ginxlog.o ginentrypage.o gindatapage.o \
	ginbtree.o ginscan.o ginget.o ginvacuum.o ginarrayproc.o \
	ginbulk.o ginfast.o ginpostinglist.o ginlogic.o

include $(top_srcdir)/src/backend/common.mk
