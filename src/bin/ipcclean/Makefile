#-------------------------------------------------------------------------
#
# Makefile for src/bin/ipcclean
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.12 2000/08/31 16:11:08 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/ipcclean
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

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
