#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.26 2001/05/08 16:28:46 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: initdb

initdb: initdb.sh $(top_builddir)/src/Makefile.global
	sed -e 's/@MULTIBYTE@/$(MULTIBYTE)/g' \
	    -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's,@SHELL@,$(SHELL),g' \
	    -e 's,@bindir@,$(bindir),g' \
	    -e 's,@datadir@,$(datadir),g' \
	  $< >$@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) initdb $(DESTDIR)$(bindir)/initdb

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/initdb

clean distclean maintainer-clean:
	rm -f initdb
