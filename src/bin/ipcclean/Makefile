#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/ipcclean/Makefile,v 1.19 2005/12/09 21:19:35 petere Exp $
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
