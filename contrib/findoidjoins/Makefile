#
# $Header: /cvsroot/pgsql/contrib/findoidjoins/Attic/Makefile,v 1.9 2000/08/31 16:08:59 petere Exp $
#

subdir = contrib/findoidjoins
top_builddir = ../..
include ../../src/Makefile.global

CFLAGS	+= -I$(libpgeasy_srcdir) -I$(libpq_srcdir)
LIBS	+= $(libpgeasy)

all: findoidjoins

install: all installdirs
	$(INSTALL_PROGRAM) findoidjoins$(X)	$(bindir)
	$(INSTALL_SCRIPT) make_oidjoins_check	$(bindir)
	$(INSTALL_DATA) README.findoidjoins	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/findoidjoins$(X) $(bindir)/make_oidjoins_check $(docdir)/contrib/README.findoidjoins

clean distclean maintainer-clean:
	rm -f findoidjoins$(X)
