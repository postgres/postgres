#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgaccess
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pgaccess/Attic/Makefile,v 1.11 2000/08/31 16:11:20 petere Exp $
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
	$(INSTALL_SCRIPT) pgaccess $(bindir)/pgaccess
	$(INSTALL_SCRIPT) main.tcl $(pgaccessdir)
	for i in lib/*.tcl; do $(INSTALL_DATA) $$i $(pgaccessdir)/lib || exit 1; done
	for i in lib/help/*.hlp; do $(INSTALL_DATA) $$i $(pgaccessdir)/lib/help || exit 1; done
	for i in lib/languages/[a-z]*; do $(INSTALL_DATA) $$i $(pgaccessdir)/lib/languages || exit 1; done
	for i in images/*.gif; do $(INSTALL_DATA) $$i $(pgaccessdir)/images || exit 1; done

installdirs:
	$(mkinstalldirs) $(bindir) $(pgaccessdir)/lib/help $(pgaccessdir)/lib/languages $(pgaccessdir)/images

uninstall:
	rm -f $(bindir)/pgaccess
	rm -rf $(pgaccessdir)

clean distclean maintainer-clean:
	rm -f pgaccess
