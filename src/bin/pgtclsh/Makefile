#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for a tclsh workalike with pgtcl commands installed
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.24 1998/10/27 21:51:54 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

#
# Include definitions from the tclConfig.sh file
#
include Makefile.tcldefs
ifeq ($(USE_TK), true)
include Makefile.tkdefs
endif

CFLAGS+= $(X_CFLAGS) -I$(SRCDIR)/interfaces/libpgtcl 

ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

LIBPGTCL= -L$(SRCDIR)/interfaces/libpgtcl -lpgtcl
LIBPQ= -L$(LIBPQDIR) -lpq

# If we are here then TCL is available
PGMS = pgtclsh
INSTPGMS = install_tcl

# Add TK targets if TK is available
ifeq ($(USE_TK), true)
PGMS += pgtksh
INSTPGMS += install_tk
endif

all: $(PGMS)

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) $(TCL_DEFS) -o $@ pgtclAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) $(TCL_LIB_SPEC) $(TCL_LIBS) $(LDFLAGS)

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) $(TK_DEFS) -o $@ pgtkAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) $(X_LIBS) $(TK_LIB_SPEC) $(TK_LIBS) \
	  $(TCL_LIB_SPEC) $(X11_LIBS) $(LDFLAGS)

install: $(INSTPGMS)

install_tcl: pgtclsh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtclsh $(BINDIR)/pgtclsh

install_tk: pgtksh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtksh $(BINDIR)/pgtksh

Makefile.tcldefs: mkMakefile.tcldefs.sh
	/bin/sh mkMakefile.tcldefs.sh

Makefile.tkdefs: mkMakefile.tkdefs.sh
	/bin/sh mkMakefile.tkdefs.sh

clean:
	rm -f *.o Makefile.t*defs pgtclsh pgtksh

dep depend:
	$(CC) -MM $(CFLAGS) *.c > depend
