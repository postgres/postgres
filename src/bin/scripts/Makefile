#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.5 2000/06/27 00:31:44 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/scripts
top_builddir = ../../..
include ../../Makefile.global

SCRIPTS := createdb dropdb createuser dropuser createlang droplang vacuumdb

all: $(SCRIPTS)

createlang: createlang.sh
	sed -e 's/__DLSUFFIX__/$(DLSUFFIX)/' $< > $@

install: all installdirs
	$(INSTALL_SCRIPT) $(SCRIPTS) $(bindir)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(addprefix $(bindir)/, $(SCRIPTS))

clean distclean maintainer-clean:
	rm -f createlang
