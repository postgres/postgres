#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/Makefile,v 1.8 1998/04/06 16:49:28 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: createdb

createdb: createdb.sh
	cp createdb.sh createdb

install: createdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f createdb

dep depend:
