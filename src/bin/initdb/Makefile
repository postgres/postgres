#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.22 2000/09/01 13:15:27 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: initdb

initdb: initdb.sh $(top_builddir)/src/Makefile.global
	rm -f $@ $@.tmp
	sed -e 's/@MULTIBYTE@/$(MULTIBYTE)/g' \
	    -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's,@bindir@,$(bindir),g' \
	    -e 's,@datadir@,$(datadir),g' \
	  $< >$@.tmp
	chmod a+x $@.tmp
	mv $@.tmp $@

install: all installdirs
	$(INSTALL_SCRIPT) initdb $(bindir)/initdb

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/initdb

clean distclean maintainer-clean:
	rm -f initdb
