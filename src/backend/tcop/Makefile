#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tcop
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/tcop/Makefile,v 1.29 2008/02/19 10:30:08 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/tcop
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= dest.o fastpath.o postgres.o pquery.o utility.o

ifneq (,$(filter $(PORTNAME),cygwin win32))
override CPPFLAGS += -DWIN32_STACK_RLIMIT=$(WIN32_STACK_RLIMIT)
endif

include $(top_srcdir)/src/backend/common.mk
