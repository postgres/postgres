#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.7 1998/04/06 16:50:36 momjian Exp $
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
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f ipcclean

dep depend:
