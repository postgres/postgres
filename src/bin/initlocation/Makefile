#-------------------------------------------------------------------------
#
# Makefile for src/bin/initlocation
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initlocation/Attic/Makefile,v 1.12 2000/09/17 13:02:32 petere Exp $
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
	$(INSTALL_SCRIPT) initlocation $(DESTDIR)$(bindir)/initlocation

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/initlocation

clean distclean maintainer-clean:
	rm -f initlocation
