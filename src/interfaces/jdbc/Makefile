#-------------------------------------------------------------------------
#
# Makefile for JDBC driver
#
# Copyright (c) 2001, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/src/interfaces/jdbc/Attic/Makefile,v 1.38 2003/02/12 06:13:04 barry Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/jdbc
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

majorversion:= $(shell echo $(VERSION) | sed 's/^\([0-9][0-9]*\)\..*$$/\1/')
minorversion:= $(shell echo $(VERSION) | sed 's/^[0-9][0-9]*\.\([0-9][0-9]*\).*$$/\1/')

build.properties: $(top_builddir)/src/Makefile.global
	@echo "# This file was created by 'make build.properties'." > build.properties
	@echo major=$(majorversion) >> build.properties
	@echo minor=$(minorversion) >> build.properties
	@echo fullversion=$(VERSION) >> build.properties
	@echo def_pgport=$(DEF_PGPORT) >> build.properties
	@echo enable_debug=$(enable_debug) >> build.properties

all: build.properties
	$(ANT) -buildfile $(srcdir)/build.xml all

install: installdirs build.properties
	$(ANT) -buildfile $(srcdir)/build.xml install \
	  -Dinstall.directory=$(javadir)

installdirs:
	$(mkinstalldirs) $(javadir) 

uninstall:
	$(ANT) -buildfile $(srcdir)/build.xml uninstall \
	  -Dinstall.directory=$(javadir)

clean distclean maintainer-clean:
	$(ANT) -buildfile $(srcdir)/build.xml clean_all

check: build.properties
	$(ANT) -buildfile $(srcdir)/build.xml test
