#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/cleardbdir
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/cleardbdir/Attic/Makefile,v 1.6 1998/04/06 00:27:55 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: cleardbdir

cleardbdir:
	cp cleardbdir.sh cleardbdir

install: cleardbdir
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean: 
	rm -f cleardbdir

dep depend:
