#-------------------------------------------------------------------------
#
# Makefile for src/pl/plpgsql (PostgreSQL's SQL procedural language)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/pl/plpgsql/Makefile,v 1.7 2001/05/24 22:33:18 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all install installdirs uninstall distprep:
	$(MAKE) -C src $@

clean distclean maintainer-clean:
	-$(MAKE) -C src $@
