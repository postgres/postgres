# $Header: /cvsroot/pgsql/src/backend/po/Attic/Makefile,v 1.2 2001/09/10 22:25:48 petere Exp $

subdir = src/backend/po
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


gettext-files:
	find $(srcdir)/.. -name '*.c' -print >$@

maintainer-clean:
	rm -f gettext-files
