#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.7 2000/08/20 11:56:29 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/scripts
top_builddir = ../../..
include ../../Makefile.global

SCRIPTS := createdb dropdb createuser dropuser createlang droplang vacuumdb

all: $(SCRIPTS)

createlang: createlang.sh
	sed -e 's:__DLSUFFIX__:$(DLSUFFIX):g' \
	    -e 's:__libdir__:$(libdir):g' \
	    $< > $@

install: all installdirs
	for i in $(SCRIPTS); do $(INSTALL_SCRIPT) $$i $(bindir) || exit; done

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(addprefix $(bindir)/, $(SCRIPTS))

clean distclean maintainer-clean:
	rm -f createlang
