#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for postgresql foreign-data wrapper
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/foreign/postgresql/Makefile,v 1.1 2008/12/19 16:25:17 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/foreign/postgresql
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME = postgresql_fdw
OBJS = postgresql_fdw.o

include $(top_srcdir)/src/Makefile.shlib

all: all-shared-lib

install: all install-lib

installdirs: installdirs-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
