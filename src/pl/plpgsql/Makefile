#-------------------------------------------------------------------------
#
# Makefile for src/pl/plpgsql (PostgreSQL's SQL procedural language)
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/pl/plpgsql/Makefile,v 1.8 2003/11/29 19:52:12 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all install installdirs uninstall distprep:
	$(MAKE) -C src $@

clean distclean maintainer-clean:
	-$(MAKE) -C src $@
