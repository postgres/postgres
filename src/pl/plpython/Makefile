# $Header: /cvsroot/pgsql/src/pl/plpython/Makefile,v 1.11.6.1 2004/01/21 19:25:11 tgl Exp $

subdir = src/pl/plpython
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# On some platforms we can only build PL/Python if libpython is a
# shared library.  Since there is no official way to determine this,
# we see if there is a file that is named like a shared library.
ifneq (,$(wildcard $(python_configdir)/libpython*$(DLSUFFIX)*))
shared_libpython = yes
endif

# If we don't have a shared library and the platform doesn't allow it
# to work without, we have to skip it.
ifneq (,$(findstring yes, $(shared_libpython)$(allow_nonpic_in_shlib)))

override CPPFLAGS := -I$(srcdir) $(python_includespec) $(CPPFLAGS)
rpath :=

NAME = plpython
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 0
OBJS = plpython.o

SHLIB_LINK = $(BE_DLLLIBS) $(python_libspec)

include $(top_srcdir)/src/Makefile.shlib


all: all-lib

install: all installdirs
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(pkglibdir)/plpython$(DLSUFFIX)
else
	@echo "*****"; \
	 echo "* PL/Python was not installed due to lack of shared library support."; \
	 echo "*****"
endif

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(pkglibdir)

uninstall:
	rm -f $(DESTDIR)$(pkglibdir)/plpython$(DLSUFFIX)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	@rm -f error.diff feature.diff error.output feature.output test.log

installcheck:
	PATH=$(bindir):$$PATH $(SHELL) $(srcdir)/test.sh

else # can't build

all:
	@echo ""; \
	 echo "*** Cannot build PL/Python because libpython is not a shared library." ; \
	 echo "*** You might have to rebuild your Python installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

endif # can't build
