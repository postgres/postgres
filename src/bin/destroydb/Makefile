#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroydb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroydb/Attic/Makefile,v 1.2 1996/11/11 13:39:47 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^_fUnKy_BINDIR_sTuFf_^$(BINDIR)^g" \
    -e "s^_fUnKy_POSTPORT_sTuFf_^$(POSTPORT)^g"

all: destroydb

destroydb:
	sed $(SEDSCRIPT) <destroydb.sh >destroydb

install: destroydb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f destroydb
