# $Header: /cvsroot/pgsql/src/backend/po/Attic/Makefile,v 1.1 2001/06/02 18:25:17 petere Exp $

subdir = src/backend/po
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


gettext-files:
	find $(srcdir)/.. -name '*.c' >$@

maintainer-clean:
	rm -f gettext-files
