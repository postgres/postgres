#-------------------------------------------------------------------------
#
# Makefile for src/bin/initlocation
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initlocation/Attic/Makefile,v 1.9 2000/06/27 00:31:00 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initlocation
top_builddir = ../../..
include ../../Makefile.global

all: initlocation

initlocation: initlocation.sh
	cp $< $@

install: all installdirs
	$(INSTALL_SCRIPT) initlocation $(bindir)/initlocation

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/initlocation

clean distclean maintainer-clean:
	rm -f initlocation
