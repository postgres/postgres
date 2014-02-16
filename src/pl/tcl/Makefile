#-------------------------------------------------------------------------
#
# Makefile for the pl/tcl procedural language
#
# src/pl/tcl/Makefile
#
#-------------------------------------------------------------------------

subdir = src/pl/tcl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := $(TCL_INCLUDE_SPEC) $(CPPFLAGS)


# Find out whether Tcl was built as a shared library --- if not, we
# can't link a shared library that depends on it, and have to forget
# about building pltcl. In Tcl 8, tclConfig.sh sets TCL_SHARED_BUILD
# for us, but in older Tcl releases it doesn't. In that case we guess
# based on the name of the Tcl library.

ifndef TCL_SHARED_BUILD
ifneq (,$(findstring $(DLSUFFIX),$(TCL_LIB_FILE)))
TCL_SHARED_BUILD=1
else
TCL_SHARED_BUILD=0
endif
endif


# On Windows, we don't link directly with the Tcl library; see below
ifneq ($(PORTNAME), win32)
SHLIB_LINK = $(TCL_LIB_SPEC) $(TCL_LIBS) -lc
endif


NAME = pltcl

OBJS = pltcl.o

DATA = pltcl.control pltcl--1.0.sql pltcl--unpackaged--1.0.sql \
       pltclu.control pltclu--1.0.sql pltclu--unpackaged--1.0.sql

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-extension=pltcl
REGRESS = pltcl_setup pltcl_queries
# where to find psql for running the tests
PSQLDIR = $(bindir)

# Tcl on win32 ships with import libraries only for Microsoft Visual C++,
# which are not compatible with mingw gcc. Therefore we need to build a
# new import library to link with.
ifeq ($(PORTNAME), win32)

tclwithver = $(subst -l,,$(filter -l%, $(TCL_LIB_SPEC)))
TCLDLL = $(dir $(TCLSH))/$(tclwithver).dll

OBJS += lib$(tclwithver).a

lib$(tclwithver).a: $(tclwithver).def
	dlltool --dllname $(tclwithver).dll --def $(tclwithver).def --output-lib lib$(tclwithver).a

$(tclwithver).def: $(TCLDLL)
	pexports $^ > $@

endif # win32


include $(top_srcdir)/src/Makefile.shlib

ifeq ($(TCL_SHARED_BUILD), 1)

all: all-lib
	$(MAKE) -C modules $@


install: all install-lib install-data
	$(MAKE) -C modules $@

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'
	$(MAKE) -C modules $@

uninstall: uninstall-lib uninstall-data
	$(MAKE) -C modules $@

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'

uninstall-data:
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(DATA)))

.PHONY: install-data uninstall-data


check: all submake
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS)

installcheck: submake
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)

.PHONY: submake
submake:
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)

else # TCL_SHARED_BUILD = 0

# Provide dummy targets for the case where we can't build the shared library.
all:
	@echo "*****"; \
	 echo "* Cannot build PL/Tcl because Tcl is not a shared library; skipping it."; \
	 echo "*****"

endif # TCL_SHARED_BUILD = 0

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	rm -rf $(pg_regress_clean_files)
ifeq ($(PORTNAME), win32)
	rm -f $(tclwithver).def
endif
	$(MAKE) -C modules $@
