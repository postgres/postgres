#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.6 2000/06/28 05:09:37 tgl Exp $
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
	for i in $(SCRIPTS); do $(INSTALL_SCRIPT) $$i $(bindir); done

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(addprefix $(bindir)/, $(SCRIPTS))

clean distclean maintainer-clean:
	rm -f createlang
