#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for doc directory to install man pages
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/man/Attic/Makefile,v 1.4 1998/04/06 01:36:20 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../../..
include ../../../Makefile.global

install: 
	-mkdir $(DESTDIR)$(POSTMANDIR)
	-mkdir $(DESTDIR)$(POSTMANDIR)/man3
	cp *.3* $(DESTDIR)$(POSTMANDIR)/man3
