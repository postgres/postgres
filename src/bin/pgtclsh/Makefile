#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgtclsh
# (a tclsh workalike with pgtcl commands installed)
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/pgtclsh/Makefile,v 1.42 2003/11/29 19:52:06 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pgtclsh
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


libpgtcl_srcdir = $(top_srcdir)/src/interfaces/libpgtcl
libpgtcl_builddir = $(top_builddir)/src/interfaces/libpgtcl
libpgtcl = -L$(libpgtcl_builddir) -lpgtcl

override CPPFLAGS := -I$(libpgtcl_srcdir) $(TK_XINCLUDES) $(TCL_INCLUDE_SPEC) $(CPPFLAGS)


# If we are here then Tcl is available
PROGRAMS = pgtclsh

# Add Tk targets if Tk is available
ifeq ($(with_tk), yes)
PROGRAMS += pgtksh
endif

all: submake $(PROGRAMS)

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) $^ $(libpgtcl) $(libpq) $(TCL_LIB_SPEC) $(TCL_LIBS) $(LDFLAGS) $(LIBS) -o $@

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) $^ $(libpgtcl) $(libpq) $(TK_LIB_SPEC) $(TK_LIBS) $(TCL_LIB_SPEC) $(LDFLAGS) $(LIBS) -o $@

.PHONY: submake
submake:
	$(MAKE) -C $(libpgtcl_builddir) all

install: all installdirs
	$(INSTALL_PROGRAM) pgtclsh $(DESTDIR)$(bindir)/pgtclsh
ifeq ($(with_tk), yes)
	$(INSTALL_PROGRAM) pgtksh $(DESTDIR)$(bindir)/pgtksh
endif

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pgtclsh $(DESTDIR)$(bindir)/pgtksh

clean distclean maintainer-clean:
	rm -f pgtclAppInit.o pgtkAppInit.o pgtclsh pgtksh
