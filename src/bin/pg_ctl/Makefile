#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_ctl
#
# Copyright (c) 1999, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/src/bin/pg_ctl/Makefile,v 1.6 2000/06/27 00:31:08 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_ctl
top_builddir = ../../..
include ../../Makefile.global

all: pg_ctl

pg_ctl: pg_ctl.sh
	cp $< $@

install: all installdirs
	$(INSTALL_SCRIPT) pg_ctl $(bindir)/pg_ctl

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/pg_ctl

clean distclean maintainer-clean:
	rm -f pg_ctl
