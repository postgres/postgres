#-------------------------------------------------------------------------
#
# Makefile for src/bin/initlocation
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/initlocation/Makefile,v 1.15 2003/11/29 19:52:04 pgsql Exp $
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
