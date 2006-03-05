#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/ipcclean/Makefile,v 1.20 2006/03/05 15:58:50 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/ipcclean
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: ipcclean

ipcclean: ipcclean.sh
	cp $< $@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) ipcclean '$(DESTDIR)$(bindir)/ipcclean'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/ipcclean'

clean distclean maintainer-clean:
	rm -f ipcclean
