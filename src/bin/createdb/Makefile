#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/Makefile,v 1.3 1996/11/14 10:24:45 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: createdb

createdb: createdb.sh
	cp createdb.sh createdb

install: createdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f createdb
