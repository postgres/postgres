#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for doc directory to install man pages
#
# Copyright (c) 1994, Regents of the University of California
# Copyright (c) 1996, Dr George D Detlefsen
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/man/Attic/Makefile,v 1.4 1998/04/06 00:31:52 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR=..
include ../Makefile.global

install:
	-mkdir -p $(POSTMANDIR)
	-mkdir -p $(POSTMANDIR)/man1
	-mkdir -p $(POSTMANDIR)/man3
	-mkdir -p $(POSTMANDIR)/man5
	-mkdir -p $(POSTMANDIR)/manl
	cp *.1* $(POSTMANDIR)/man1
	cp *.3* $(POSTMANDIR)/man3
	cp *.5* $(POSTMANDIR)/man5
	cp *.l* $(POSTMANDIR)/manl

clean:

all:

dep depend:
