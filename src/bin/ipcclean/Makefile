#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.14 2000/09/17 13:02:33 petere Exp $
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
