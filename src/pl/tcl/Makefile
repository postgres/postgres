#-------------------------------------------------------------------------
#
# Makefile for the pltcl shared object
#
# $Header: /cvsroot/pgsql/src/pl/tcl/Makefile,v 1.36 2002/01/21 03:27:46 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/tcl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

-include Makefile.tcldefs

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


# Change following to how shared library that contains references to
# libtcl must get built on your system. Since these definitions come
# from the tclConfig.sh script, they should work if the shared build
# of tcl was successful on this system. However, tclConfig.sh lies to
# us a little bit (at least in versions 7.6 through 8.0.4) --- it
# doesn't mention -lc in TCL_LIBS, but you still need it on systems
# that want to hear about dependent libraries...

ifneq ($(TCL_SHLIB_LD_LIBS),)
# link command for a shared lib must mention shared libs it uses
SHLIB_EXTRA_LIBS=$(TCL_LIBS) -lc
else
ifeq ($(PORTNAME), hpux)
# link command for a shared lib must mention shared libs it uses,
# even though Tcl doesn't think so...
SHLIB_EXTRA_LIBS=$(TCL_LIBS) -lc
else
# link command for a shared lib must NOT mention shared libs it uses
SHLIB_EXTRA_LIBS=
endif
endif

# Suppress bogus soname switch that RedHat RPMs put into tclConfig.sh
override TCL_SHLIB_LD := $(shell echo "$(TCL_SHLIB_LD)" | sed 's/-Wl,-soname[^ ]*//')


%$(TCL_SHLIB_SUFFIX): %.o
	$(TCL_SHLIB_LD) -o $@ $< $(TCL_LIB_SPEC) $(SHLIB_EXTRA_LIBS)


CC = $(TCL_CC)

# Since we are using Tcl's choice of C compiler, which might not be
# the same one selected for Postgres, do NOT use CFLAGS from
# Makefile.global. Instead use TCL's CFLAGS plus necessary -I
# directives.

# Can choose either TCL_CFLAGS_OPTIMIZE or TCL_CFLAGS_DEBUG here, as
# needed
override CPPFLAGS += $(TCL_DEFS)
override CFLAGS = $(TCL_CFLAGS_OPTIMIZE) $(TCL_SHLIB_CFLAGS)


#
# DLOBJS is the dynamically-loaded object file.
#
DLOBJS= pltcl$(DLSUFFIX)

INFILES= $(DLOBJS) 

#
# plus exports files
#
ifdef EXPSUFF
INFILES+= $(DLOBJS:.o=$(EXPSUFF))
endif


# Provide dummy targets for the case where we can't build the shared library.

ifeq ($(TCL_SHARED_BUILD), 1)

all: $(INFILES)
	$(MAKE) -C modules $@

pltcl$(DLSUFFIX): pltcl.o

install: all installdirs
	$(INSTALL_SHLIB) $(DLOBJS) $(DESTDIR)$(pkglibdir)/$(DLOBJS)
	$(MAKE) -C modules $@

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(pkglibdir)
	$(MAKE) -C modules $@

uninstall:
	rm -f $(DESTDIR)$(pkglibdir)/$(DLOBJS)
	$(MAKE) -C modules $@

else # TCL_SHARED_BUILD = 0

all install:
	@echo "*****"; \
	 echo "* Cannot build pltcl because Tcl is not a shared library; skipping it."; \
	 echo "*****"

endif # TCL_SHARED_BUILD = 0

Makefile.tcldefs: mkMakefile.tcldefs.sh
	$(SHELL) $< '$(TCL_CONFIG_SH)' '$@'

clean distclean maintainer-clean:
	rm -f $(INFILES) pltcl.o Makefile.tcldefs
	$(MAKE) -C modules $@
