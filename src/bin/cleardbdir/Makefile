#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/cleardbdir
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/cleardbdir/Attic/Makefile,v 1.2 1996/11/11 13:39:31 bryanh Exp $
#
#-------------------------------------------------------------------------

all: cleardbdir

cleardbdir:
	cp cleardbdir.sh cleardbdir

install: cleardbdir
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean: 
	rm -f cleardbdir

