#-------------------------------------------------------------------------
#
# Makefile for bin/ipcclean
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/ipcclean/Attic/Makefile,v 1.10 2000/04/14 23:43:43 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: ipcclean

ipcclean: ipcclean.sh
	cp $< $@

install: ipcclean
	$(INSTALL) $(INSTL_EXE_OPTS) $^ $(BINDIR)

clean:
	rm -f ipcclean

dep depend:
