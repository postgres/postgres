#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.9 1999/12/08 10:29:46 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^PG_OPT_IPCCLEANPATH_PARAM^$(IPCSDIR)^g" 

all: ipcclean

ipcclean:
	sed $(SEDSCRIPT) <ipcclean.sh >ipcclean

install: ipcclean
	$(INSTALL) $(INSTL_EXE_OPTS) $+ $(BINDIR)

clean:
	rm -f ipcclean

dep depend:
