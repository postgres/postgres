#-------------------------------------------------------------------------
#
# Makefile for the pltcl shared object
#
# $Header: /cvsroot/pgsql/src/pl/tcl/Makefile,v 1.39 2002/12/30 17:19:54 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/tcl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := $(CPPFLAGS) $(TCL_INCLUDE_SPEC)


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


# The following attempts to figure out what libraries need to be
# linked with pltcl.  The information comes from the tclConfig.sh
# file, but it's mostly bogus.  This just might work.

ifneq ($(TCL_SHLIB_LD_LIBS),)
# link command for a shared lib must mention shared libs it uses
SHLIB_LINK = $(TCL_LIB_SPEC) $(TCL_LIBS) -lc
else
ifeq ($(PORTNAME), hpux)
# link command for a shared lib must mention shared libs it uses,
# even though Tcl doesn't think so...
SHLIB_LINK = $(TCL_LIB_SPEC) $(TCL_LIBS) -lc
else
# link command for a shared lib must NOT mention shared libs it uses
SHLIB_LINK = $(TCL_LIB_SPEC)
endif
endif


NAME = pltcl
SO_MAJOR_VERSION = 2
SO_MINOR_VERSION = 0
OBJS = pltcl.o

include $(top_srcdir)/src/Makefile.shlib

ifeq ($(TCL_SHARED_BUILD), 1)

all: all-lib
	$(MAKE) -C modules $@

install: all installdirs
	$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)
	$(MAKE) -C modules $@

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(pkglibdir)
	$(MAKE) -C modules $@

uninstall:
	rm -f $(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)
	$(MAKE) -C modules $@

else # TCL_SHARED_BUILD = 0

# Provide dummy targets for the case where we can't build the shared library.
all:
	@echo "*****"; \
	 echo "* Cannot build pltcl because Tcl is not a shared library; skipping it."; \
	 echo "*****"

endif # TCL_SHARED_BUILD = 0

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	$(MAKE) -C modules $@
