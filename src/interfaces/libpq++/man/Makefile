#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for doc directory to install man pages
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/man/Attic/Makefile,v 1.1 1996/11/13 06:36:38 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

install: 
	-mkdir -p $(POSTMANDIR)
	-mkdir $(POSTMANDIR)/man3
	cp man/*.3* $(POSTMANDIR)/man3

