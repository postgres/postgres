#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgaccess
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pgaccess/Attic/Makefile,v 1.13 2000/09/25 22:22:56 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pgaccess
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

pgaccessdir = $(datadir)/pgaccess

all: pgaccess

pgaccess: pgaccess.sh $(top_builddir)/src/Makefile.global
	sed -e 's,@WISH@,$(WISH),g' \
	    -e 's,@PGACCESSHOME@,$(pgaccessdir),g' \
	  $< >$@
	chmod a+x $@

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
