#
# $Header: /cvsroot/pgsql/contrib/tips/Attic/Makefile,v 1.4 2000/07/09 13:13:43 petere Exp $
#

subdir = contrib/tips
top_builddir = ../..
include ../../src/Makefile.global

all:

install: installdirs
	$(INSTALL_DATA) README.apachelog $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib

uninstall:
	rm -f $(docdir)/contrib/README.apachelog
