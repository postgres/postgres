#-------------------------------------------------------------------------
#
# Makefile for src/bin/initlocation
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initlocation/Attic/Makefile,v 1.11 2000/09/08 18:29:22 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initlocation
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: initlocation

initlocation: initlocation.sh
	cp $< $@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) initlocation $(bindir)/initlocation

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/initlocation

clean distclean maintainer-clean:
	rm -f initlocation
