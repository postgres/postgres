# $PostgreSQL: pgsql/src/pl/plpython/Makefile,v 1.32 2009/01/15 13:49:56 petere Exp $

subdir = src/pl/plpython
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# On some platforms we can only build PL/Python if libpython is a
# shared library.  Since there is no official way to determine this
# (at least not in pre-2.3 Python), we see if there is a file that is
# named like a shared library.
ifneq (,$(wildcard $(python_libdir)/libpython*$(DLSUFFIX)*))
shared_libpython = yes
endif

# Windows needs to convert backslashed paths to normal slashes,
# and we have to remove -lpython from the link since we are building our own
ifeq ($(PORTNAME), win32)
shared_libpython = yes
python_includespec := $(subst \,/,$(python_includespec))
override python_libspec =
endif

# Darwin (OS X) does supply a .dylib, but the above test doesn't match that.
ifeq ($(PORTNAME), darwin)
shared_libpython = yes
endif

# If we don't have a shared library and the platform doesn't allow it
# to work without, we have to skip it.
ifneq (,$(findstring yes, $(shared_libpython)$(allow_nonpic_in_shlib)))

override CPPFLAGS := -I$(srcdir) $(python_includespec) $(CPPFLAGS)

rpathdir = $(python_libdir)

NAME = plpython
OBJS = plpython.o


# Python on win32 ships with import libraries only for Microsoft Visual C++,
# which are not compatible with mingw gcc. Therefore we need to build a
# new import library to link with.
ifeq ($(PORTNAME), win32)
pytverstr=$(subst .,,${python_version})
OBJS += libpython${pytverstr}.a
libpython${pytverstr}.a: python${pytverstr}.def
	dlltool --dllname python${pytverstr}.dll --def python${pytverstr}.def --output-lib  libpython${pytverstr}.a
WD=$(subst \,/,$(WINDIR))
python${pytverstr}.def: $(WD)/system32/python${pytverstr}.dll
	pexports $(WD)/system32/python${pytverstr}.dll > python${pytverstr}.def
endif


SHLIB_LINK = $(python_libspec) $(python_additional_libs) $(filter -lintl,$(LIBS))

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=plpythonu
REGRESS = plpython_schema plpython_populate plpython_function plpython_test plpython_error plpython_drop
# where to find psql for running the tests
PSQLDIR = $(bindir)

include $(top_srcdir)/src/Makefile.shlib


all: all-lib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress --inputdir=$(srcdir) --psqldir="$(PSQLDIR)" $(REGRESS_OPTS) $(REGRESS)

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	rm -rf results
	rm -f regression.diffs regression.out
ifeq ($(PORTNAME), win32)
	rm -f python${pytverstr}.def
endif

else # can't build

all:
	@echo ""; \
	 echo "*** Cannot build PL/Python because libpython is not a shared library." ; \
	 echo "*** You might have to rebuild your Python installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

endif # can't build
