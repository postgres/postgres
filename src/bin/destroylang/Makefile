#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroylang
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroylang/Attic/Makefile,v 1.1 1999/05/20 16:50:02 wieck Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: destroylang

destroylang: destroylang.sh
	cp destroylang.sh destroylang

install: destroylang
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f destroylang

dep depend:
