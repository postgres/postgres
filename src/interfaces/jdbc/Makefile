#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/jdbc/Attic/Makefile,v 1.27 2001/03/05 09:39:53 peter Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/jdbc
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all distprep:
	@$(ANT) -buildfile $(top_builddir)/build.xml

install:
	@$(ANT) -Dinstall.directory=$(DESTDIR)$(libdir)/java \
		-buildfile $(top_builddir)/build.xml \
		install

installdirs uninstall dep depend:
	@echo Nothing for JDBC

clean distclean maintainer-clean:
	@$(ANT) -buildfile $(top_builddir)/build.xml clean

