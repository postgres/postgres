#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for a tclsh workalike with pgtcl commands installed
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.20 1998/10/12 04:32:22 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

#
# Include definitions from the tclConfig.sh file
#
include Makefile.tcltkdefs

CFLAGS+= $(X_CFLAGS) -I$(SRCDIR)/interfaces/libpgtcl 

ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

# try to find libpgtcl.a in either directory
LIBPGTCL= -L$(SRCDIR)/interfaces/libpgtcl -lpgtcl
LIBPQ= -L$(LIBPQDIR) -lpq

all: pgtclsh pgtksh

Makefile.tcltkdefs:
	/bin/sh mkMakefile.tcltkdefs.sh

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) $(TCL_DEFS) -o $@ pgtclAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) $(TCL_LIB_SPEC) $(TCL_LIBS) $(LDFLAGS)

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) $(TK_DEFS) -o $@ pgtkAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) $(X_LIBS) $(TK_LIB_SPEC) $(TK_LIBS) \
	  $(TCL_LIB_SPEC) $(TCL_LIB) $(X11_LIBS) $(LDFLAGS)

install: pgtclsh pgtksh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtclsh $(BINDIR)/pgtclsh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtksh $(BINDIR)/pgtksh

clean:
	rm -f pgtclAppInit.o pgtkAppInit.o pgtclsh pgtksh

dep depend:
	$(CC) -MM $(CFLAGS) *.c > depend
