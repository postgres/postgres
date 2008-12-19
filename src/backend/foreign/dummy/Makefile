#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for dummy foreign-data wrapper
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/foreign/dummy/Makefile,v 1.1 2008/12/19 16:25:17 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/foreign/dummy
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME = dummy_fdw
OBJS = dummy_fdw.o

include $(top_srcdir)/src/Makefile.shlib

all: all-shared-lib

install: all install-lib

installdirs: installdirs-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
