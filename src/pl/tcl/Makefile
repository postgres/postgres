#-------------------------------------------------------------------------
#
# Makefile for the pltcl shared object
#
# $Header: /cvsroot/pgsql/src/pl/tcl/Makefile,v 1.40.2.1 2004/01/21 19:25:11 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/tcl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS += $(TCL_INCLUDE_SPEC)


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


SHLIB_LINK = $(BE_DLLLIBS) $(TCL_LIB_SPEC) $(TCL_LIBS) -lc

NAME = pltcl
SO_MAJOR_VERSION = 2
SO_MINOR_VERSION = 0
OBJS = pltcl.o

include $(top_srcdir)/src/Makefile.shlib

ifeq ($(TCL_SHARED_BUILD), 1)

all: all-lib
	$(MAKE) -C modules $@

install: all installdirs
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)
else
	@echo "*****"; \
	 echo "* PL/Tcl was not installed due to lack of shared library support."; \
	 echo "*****"
endif
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
	 echo "* Cannot build PL/Tcl because Tcl is not a shared library; skipping it."; \
	 echo "*****"

endif # TCL_SHARED_BUILD = 0

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	$(MAKE) -C modules $@
