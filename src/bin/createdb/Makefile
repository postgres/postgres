#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createdb/Attic/Makefile,v 1.2 1996/11/11 13:39:34 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^_fUnKy_BINDIR_sTuFf_^$(BINDIR)^g" \
    -e "s^_fUnKy_POSTPORT_sTuFf_^$(POSTPORT)^g"

all: createdb

createdb:
	sed $(SEDSCRIPT) <createdb.sh >createdb

install: createdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f createdb
