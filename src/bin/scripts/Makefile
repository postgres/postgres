#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.15 2002/06/20 20:29:42 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SCRIPTS := createdb dropdb createuser dropuser createlang droplang vacuumdb

all: $(SCRIPTS)

# we don't need this now, but we may need it some day, bjm  2002-01-03
createlang: createlang.sh
	cp $< $@
	chmod a+x $@

install: all installdirs
	for i in $(filter-out createlang, $(SCRIPTS)); do \
	  $(INSTALL_SCRIPT) $(srcdir)/$$i $(DESTDIR)$(bindir)/$$i || exit; \
	done
	$(INSTALL_SCRIPT) createlang $(DESTDIR)$(bindir)/createlang

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, $(SCRIPTS))

clean distclean maintainer-clean:
	rm -f createlang
