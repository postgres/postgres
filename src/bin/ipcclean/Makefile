#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.11 2000/06/27 00:31:05 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/ipcclean
top_builddir = ../../..
include ../../Makefile.global

all: ipcclean

ipcclean: ipcclean.sh
	cp $< $@

install: all installdirs
	$(INSTALL_SCRIPT) ipcclean $(bindir)/ipcclean

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/ipcclean

clean distclean maintainer-clean:
	rm -f ipcclean
