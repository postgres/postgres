#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgaccess
#
# Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pgaccess/Attic/Makefile,v 1.16 2001/02/18 18:34:01 momjian Exp $
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
	    -e 's,@PGLIB@,$(libdir),g' \
	    -e 's,@DEF_PGPORT@,$(DEF_PGPORT),g' \
	  $< >$@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) pgaccess $(DESTDIR)$(bindir)/pgaccess
	$(INSTALL_SCRIPT) $(srcdir)/main.tcl $(DESTDIR)$(pgaccessdir)
	for i in $(srcdir)/lib/*.tcl; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/lib || exit 1; done
	for i in $(srcdir)/lib/help/*.hlp; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/lib/help || exit 1; done
	for i in $(srcdir)/lib/languages/[a-z]*; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/lib/languages || exit 1; done
	for i in $(srcdir)/images/*.gif; do $(INSTALL_DATA) $$i $(DESTDIR)$(pgaccessdir)/images || exit 1; done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir) $(DESTDIR)$(pgaccessdir)/lib/help $(DESTDIR)$(pgaccessdir)/lib/languages $(DESTDIR)$(pgaccessdir)/images

uninstall:
	rm -f $(DESTDIR)$(bindir)/pgaccess
	rm -rf $(DESTDIR)$(pgaccessdir)

clean distclean maintainer-clean:
	rm -f pgaccess
