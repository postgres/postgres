#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgtclsh
# (a tclsh workalike with pgtcl commands installed)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.32 2000/09/17 13:02:42 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pgtclsh
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

#
# Include definitions from the tclConfig.sh file
#
-include Makefile.tcldefs
ifeq ($(USE_TK), true)
-include Makefile.tkdefs
endif

libpgtcl_srcdir = $(top_srcdir)/src/interfaces/libpgtcl
libpgtcl_builddir = $(top_builddir)/src/interfaces/libpgtcl
libpgtcl = -L$(libpgtcl_builddir) -lpgtcl

CFLAGS+= $(X_CFLAGS) -I$(libpgtcl_srcdir)


# If we are here then TCL is available
PROGRAMS = pgtclsh

# Add TK targets if TK is available
ifeq ($(USE_TK), true)
PROGRAMS += pgtksh
endif

all: submake $(PROGRAMS)

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) $(TCL_DEFS) -o $@ $< \
	  $(libpgtcl) $(libpq) $(TCL_LIB_SPEC) $(TCL_LIBS) $(LDFLAGS)

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) $(TK_DEFS) -o $@ $< \
	  $(libpgtcl) $(libpq) $(X_LIBS) $(TK_LIB_SPEC) $(TK_LIBS) \
	  $(TCL_LIB_SPEC) $(X11_LIBS) $(LDFLAGS)

.PHONY: submake
submake:
	$(MAKE) -C $(libpgtcl_builddir) all

install: all installdirs
	$(INSTALL_PROGRAM) pgtclsh $(DESTDIR)$(bindir)/pgtclsh
ifeq ($(USE_TK), true)
	$(INSTALL_PROGRAM) pgtksh $(DESTDIR)$(bindir)/pgtksh
endif

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pgtclsh $(DESTDIR)$(bindir)/pgtksh

Makefile.tcldefs: mkMakefile.tcldefs.sh
	$(SHELL) $< '$(TCL_CONFIG_SH)' '$@'

Makefile.tkdefs: mkMakefile.tkdefs.sh
	$(SHELL) $< '$(TK_CONFIG_SH)' '$@'

clean distclean maintainer-clean:
	rm -f *.o Makefile.tcldefs Makefile.tkdefs pgtclsh pgtksh

dep depend:
	$(CC) -MM $(CFLAGS) *.c > depend
