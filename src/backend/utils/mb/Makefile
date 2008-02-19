#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/mb/Makefile,v 1.24 2008/02/19 10:30:09 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = encnames.o conv.o mbutils.o wchar.o wstrcmp.o wstrncmp.o

include $(top_srcdir)/src/backend/common.mk

clean distclean maintainer-clean:
	$(MAKE) -C conversion_procs $@
