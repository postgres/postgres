#
# $Header: /cvsroot/pgsql/contrib/unixdate/Attic/Makefile,v 1.3 2000/07/09 13:13:50 petere Exp $
#

subdir = contrib/unixdate
top_builddir = ../..
include ../../src/Makefile.global

all:

install: installdirs
	$(INSTALL_DATA) unixdate.sql $(datadir)/contrib

uninstall:
	rm -f $(datadir)/contrib/unixdate.sql

installdirs:
	$(mkinstalldirs) $(datadir)/contrib
