#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroydb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroydb/Attic/Makefile,v 1.6 1998/04/06 00:28:25 momjian Exp $
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

dep depend:
