#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.2 1996/11/11 13:40:39 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^_fUnKy_IPCCLEANPATH_sTuFf_^$(IPCSDIR)^g" 

all: ipcclean

ipcclean:
	sed $(SEDSCRIPT) <ipcclean.sh >ipcclean

install: ipcclean
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f ipcclean
