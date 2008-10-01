#-------------------------------------------------------------------------
#
# Makefile for the pltcl shared object
#
# $PostgreSQL: pgsql/src/pl/tcl/Makefile,v 1.52 2008/10/01 22:38:56 petere Exp $
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


SHLIB_LINK = $(TCL_LIB_SPEC)
ifneq ($(PORTNAME), win32)
SHLIB_LINK += $(TCL_LIBS) -lc
endif

NAME = pltcl
OBJS = pltcl.o

REGRESS_OPTS = --dbname=$(PL_TESTDB) --load-language=pltcl
REGRESS = pltcl_setup pltcl_queries
# where to find psql for running the tests
PSQLDIR = $(bindir)

include $(top_srcdir)/src/Makefile.shlib

ifeq ($(TCL_SHARED_BUILD), 1)

all: all-lib
	$(MAKE) -C modules $@

install: all installdirs install-lib
	$(MAKE) -C modules $@

installdirs: installdirs-lib
	$(MAKE) -C modules $@

uninstall: uninstall-lib
	$(MAKE) -C modules $@

installcheck: submake
	$(top_builddir)/src/test/regress/pg_regress --psqldir=$(PSQLDIR) $(REGRESS_OPTS) $(REGRESS)

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
	rm -rf results
	rm -f regression.diffs regression.out
	$(MAKE) -C modules $@
