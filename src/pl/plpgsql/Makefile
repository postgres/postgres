#-------------------------------------------------------------------------
#
# Makefile for src/pl/plpgsql (PostgreSQL's SQL procedural language)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/pl/plpgsql/Makefile,v 1.3 2000/06/27 00:31:56 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql
top_builddir = ../../..
include ../../Makefile.global

all install installdirs uninstall:
	$(MAKE) -C src $@

clean distclean maintainer-clean:
	-$(MAKE) -C src $@
