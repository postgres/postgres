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
#    $Header: /cvsroot/pgsql/src/man/Attic/Makefile,v 1.6 1998/04/06 02:03:03 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR=..
include ../Makefile.global

install:
	-mkdir $(DESTDIR)$(POSTMANDIR)
	-mkdir $(DESTDIR)$(POSTMANDIR)/man1
	-mkdir $(DESTDIR)$(POSTMANDIR)/man3
	-mkdir $(DESTDIR)$(POSTMANDIR)/man5
	-mkdir $(DESTDIR)$(POSTMANDIR)/manl
	cp *.1* $(DESTDIR)$(POSTMANDIR)/man1
	cp *.3* $(DESTDIR)$(POSTMANDIR)/man3
	cp *.5* $(DESTDIR)$(POSTMANDIR)/man5
	cp *.l* $(DESTDIR)$(POSTMANDIR)/manl

clean:

all:

dep depend:
