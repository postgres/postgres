# $Header: /cvsroot/pgsql/src/pl/plpython/Makefile,v 1.5 2001/05/12 17:49:32 petere Exp $

subdir = src/pl/plpython
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I$(srcdir) -I$(python_includedir) $(CPPFLAGS)

NAME = plpython
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
OBJS = plpython.o


SHLIB_LINK += $(python_libspec)

# If you have not patched the dynamic loader files as described in the
# README you will have to link these in manually.  You may have to add
# more modules.  If you have patched the loader, override
# EXTRA_MODULES to be empty.

# version 2.x
ifneq (,$(findstring 2.,$(python_version)))
EXTRA_MODULES = array cmath errno math md5 operator pcre sha time
endif

# version 1.x
ifneq (,$(findstring 1.,$(python_version)))
EXTRA_MODULES = arraymodule cmathmodule errnomodule mathmodule md5module operator shamodule timemodule
endif

SHLIB_LINK += $(addprefix $(python_dynlibdir)/, $(addsuffix $(DLSUFFIX), $(EXTRA_MODULES)))

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
