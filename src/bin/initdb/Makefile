#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.2 1996/11/11 13:40:25 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^_fUnKy_BINDIR_sTuFf_^$(BINDIR)^g" \
    -e "s^_fUnKy_LIBDIR_sTuFf_^$(LIBDIR)^g" \
    -e "s^_fUnKy_DATADIR_sTuFf_^$(DATADIR)^g" \
    -e "s^_fUnKy_NAMEDATALEN_sTuFf_^$(NAMEDATALEN)^g" \
    -e "s^_fUnKy_OIDNAMELEN_sTuFf_^$(OIDNAMELEN)^g" \
    -e "s^_fUnKy_POSTPORT_sTuFf_^$(POSTPORT)^g"

all: initdb

initdb:
	sed $(SEDSCRIPT) <initdb.sh >initdb

install: initdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f initdb
