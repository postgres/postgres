#-------------------------------------------------------------------------
#
# Makefile for contributed retep tools
#
# Copyright (c) 2001, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/contrib/retep/Attic/Makefile,v 1.1 2001/07/06 23:07:20 petere Exp $
#
#-------------------------------------------------------------------------

subdir = contrib/retep
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

all:
	$(ANT) -buildfile $(srcdir)/build.xml all

install: installdirs
	$(ANT) -buildfile $(srcdir)/build.xml install \
	  -Dinstall.directory=$(javadir)

installdirs:
	$(mkinstalldirs) $(javadir)

uninstall:
	$(ANT) -buildfile $(srcdir)/build.xml uninstall \
	  -Dinstall.directory=$(javadir)

clean distclean maintainer-clean:
	$(ANT) -buildfile $(srcdir)/build.xml clean
