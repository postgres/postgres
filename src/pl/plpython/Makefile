# $Header: /cvsroot/pgsql/src/pl/plpython/Makefile,v 1.4 2001/05/12 01:30:30 petere Exp $

subdir = src/pl/plpython
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# These things ought to go into $(top_srcdir)/config/python.m4 sometime.

PYTHON_VERSION := $(shell $(PYTHON) -c  'import sys; print sys.version[:3]')
PYTHON_INCLUDE := $(shell $(PYTHON) -c 'import sys; print sys.prefix')/include/python$(PYTHON_VERSION)

override CPPFLAGS := -I$(srcdir) -I$(PYTHON_INCLUDE) $(CPPFLAGS)

PYTHON_LIB := $(shell $(PYTHON) -c 'import sys; print sys.exec_prefix')/lib/python$(PYTHON_VERSION)

NAME = plpython
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
OBJS = plpython.o

# This static version might work on most ELF systems...
SHLIB_LINK += $(PYTHON_LIB)/config/libpython$(PYTHON_VERSION).a
# ...otherwise you need a shared version, but you need to build that yourself.
#SHLIB_LINK += -lpython$(PYTHON_VERSION)

# Python uses this.  Should become a configure check.
SHLIB_LINK += -lpthread

# Python 2 seems to want libdb.
#SHLIB_LINK += -ldb2


# Hopefully you won't need this utter crap.  But if you can't patch
# the appropriate dynloader file, try this.  You may have to add other
# modules.
#
#DLDIR=$(PYTHON_LIB)/lib-dynload
#DLHACK=$(DLDIR)/arraymodule.so $(DLDIR)/timemodule.so $(DLDIR)/cmathmodule.so $(DLDIR)/errnomodule.so $(DLDIR)/mathmodule.so $(DLDIR)/md5module.so $(DLDIR)/operator.so $(DLDIR)/shamodule.so
#SHLIB_LINK += $(DLDIR)

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
