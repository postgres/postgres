#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/scripts
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.3 1999/12/08 10:29:55 momjian Exp $
#
#-------------------------------------------------------------------------

.SUFFIXES:

SRCDIR=../..
include ../../Makefile.global

SCRIPTS=createdb dropdb createuser dropuser createlang droplang vacuumdb

all: $(SCRIPTS)

createdb:
dropdb:
createuser:
dropuser:

createlang: createlang.sh
	sed -e 's/__DLSUFFIX__/$(DLSUFFIX)/' createlang.sh > createlang
droplang:

vacuumdb:

install: $(SCRIPTS)
	$(INSTALL) $(INSTL_EXE_OPTS) $+ $(BINDIR)

clean:
	rm -f createlang

dep depend:
