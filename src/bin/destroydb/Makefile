#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroydb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroydb/Attic/Makefile,v 1.5 1998/04/05 22:00:08 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: destroydb

destroydb: destroydb.sh
	cp destroydb.sh destroydb

install: destroydb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f destroydb

dep:
