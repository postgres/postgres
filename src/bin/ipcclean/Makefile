#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/ipcclean/Makefile,v 1.17 2003/11/29 19:52:04 pgsql Exp $
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
	$(INSTALL_SCRIPT) ipcclean $(DESTDIR)$(bindir)/ipcclean

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/ipcclean

clean distclean maintainer-clean:
	rm -f ipcclean
