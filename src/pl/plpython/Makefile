# $Header: /cvsroot/pgsql/src/pl/plpython/Makefile,v 1.6 2001/05/14 22:06:50 petere Exp $

subdir = src/pl/plpython
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I$(srcdir) -I$(python_includedir) $(CPPFLAGS)

NAME = plpython
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
OBJS = plpython.o

SHLIB_LINK += $(python_libspec)

include $(top_srcdir)/src/Makefile.shlib


all: all-lib

install: all installdirs
	$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(libdir)/plpython$(DLSUFFIX)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir)

uninstall:
	rm -f $(DESTDIR)$(libdir)/plpython$(DLSUFFIX)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	@rm -f error.diff feature.diff error.output feature.output test.log

installcheck:
	PATH=$(bindir):$$PATH $(SHELL) $(srcdir)/test.sh
