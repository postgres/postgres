#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.18 2000/06/27 00:30:53 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initdb
top_builddir = ../../..
include ../../Makefile.global

all: initdb

initdb: initdb.sh
	sed -e 's/__MULTIBYTE__/$(MULTIBYTE)/g' < $< > $@

install: all installdirs
	$(INSTALL_SCRIPT) initdb $(bindir)/initdb

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/initdb

clean distclean maintainer-clean:
	rm -f initdb
