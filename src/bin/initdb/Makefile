#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.21 2000/08/31 16:11:06 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: initdb

initdb: initdb.sh $(top_builddir)/src/Makefile.global
	sed -e 's/__MULTIBYTE__/$(MULTIBYTE)/g' \
	    -e 's/__VERSION__/$(VERSION)/g' \
	    -e 's:__bindir__:$(bindir):g' \
	    -e 's:__datadir__:$(datadir):g' \
	  < $< > $@

install: all installdirs
	$(INSTALL_SCRIPT) initdb $(bindir)/initdb

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/initdb

clean distclean maintainer-clean:
	rm -f initdb
