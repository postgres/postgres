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
#    $Header: /cvsroot/pgsql/doc/Makefile,v 1.1.1.1 1996/08/18 22:14:17 scrappy Exp $
#
#-------------------------------------------------------------------------

MKDIR=	../src/mk
include $(MKDIR)/postgres.mk

CLEANFILES=*

install-man:
	-mkdir -p $(POSTMANDIR)
	-mkdir $(POSTMANDIR)/man1
	-mkdir $(POSTMANDIR)/man3
	-mkdir $(POSTMANDIR)/man5
	-mkdir $(POSTMANDIR)/manl
	cp man/*.1* $(POSTMANDIR)/man1
	cp man/*.3* $(POSTMANDIR)/man3
	cp man/*.5* $(POSTMANDIR)/man5
	cp man/*.l* $(POSTMANDIR)/manl

install:: install-man

