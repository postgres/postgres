#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroydb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroydb/Attic/Makefile,v 1.3 1996/11/14 10:25:10 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: destroydb

destroydb: destroydb.sh
	cp destroydb.sh destroydb

install: destroydb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f destroydb
