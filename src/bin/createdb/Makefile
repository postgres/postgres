#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/Makefile,v 1.9 1998/07/24 03:32:04 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: createdb

createdb: createdb.sh
	sed 's/__MB__/$(MB)/' createdb.sh > createdb

install: createdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f createdb

dep depend:
