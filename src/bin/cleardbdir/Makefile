#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/cleardbdir
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/cleardbdir/Attic/Makefile,v 1.4 1996/11/24 03:55:19 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: cleardbdir

cleardbdir:
	cp cleardbdir.sh cleardbdir

install: cleardbdir
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean: 
	rm -f cleardbdir

dep:
