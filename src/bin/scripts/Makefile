#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/scripts
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.1 1999/12/04 04:53:21 momjian Exp $
#
#-------------------------------------------------------------------------

.SUFFIXES:

SRCDIR=../..
include ../../Makefile.global

SCRIPTS=createdb dropdb createuser dropuser vacuumdb

all: $(SCRIPTS)

createdb:
dropdb:
createuser:
dropuser:
vacuumdb:

install: $(SCRIPTS)
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$(X)$<

clean:
dep depend:
