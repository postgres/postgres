#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgaccess
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pgaccess/Attic/Makefile,v 1.12 2000/09/17 13:02:41 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pgaccess
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

pgaccessdir = $(datadir)/pgaccess

all: pgaccess

pgaccess: pgaccess.sh
	sed -e 's,__wish__,$(WISH),' -e 's,__PGACCESSHOME__,$(pgaccessdir),' < $< > $@

install: all installdirs
	$(INSTALL_SCRIPT) pgaccess $(DESTDIR)$(bindir)/pgaccess
	$(INSTALL_SCRIPT) main.tcl $(DESTDIR)$(pgaccessdir)
	for i in lib/*.tcl; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/lib || exit 1; done
	for i in lib/help/*.hlp; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/lib/help || exit 1; done
	for i in lib/languages/[a-z]*; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/lib/languages || exit 1; done
	for i in images/*.gif; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/images || exit 1; done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir) $(DESTDIR)$(pgaccessdir)/lib/help $(DESTDIR)$(pgaccessdir)/lib/languages $(DESTDIR)$(pgaccessdir)/images

uninstall:
	rm -f $(DESTDIR)$(bindir)/pgaccess
	rm -rf $(DESTDIR)$(pgaccessdir)

clean distclean maintainer-clean:
	rm -f pgaccess
