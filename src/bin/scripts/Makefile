#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.9 2000/09/08 18:29:28 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SCRIPTS := createdb dropdb createuser dropuser createlang droplang vacuumdb

all: $(SCRIPTS)

createlang: createlang.sh
	sed -e 's/@DLSUFFIX@/$(DLSUFFIX)/g' \
	    -e 's,@libdir@,$(libdir),g' \
	  $< >$@
	chmod a+x $@

install: all installdirs
	for i in $(SCRIPTS); do $(INSTALL_SCRIPT) $$i $(bindir) || exit; done

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(addprefix $(bindir)/, $(SCRIPTS))

clean distclean maintainer-clean:
	rm -f createlang
