#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/fmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/fmgr/Makefile,v 1.18 2008/02/19 10:30:08 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/fmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dfmgr.o fmgr.o funcapi.o

override CPPFLAGS += -DDLSUFFIX=\"$(DLSUFFIX)\"

include $(top_srcdir)/src/backend/common.mk
