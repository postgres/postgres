#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.13 2000/09/08 18:29:23 petere Exp $
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
	$(INSTALL_SCRIPT) ipcclean $(bindir)/ipcclean

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/ipcclean

clean distclean maintainer-clean:
	rm -f ipcclean
