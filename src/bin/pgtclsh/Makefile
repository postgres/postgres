#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for a tclsh workalike with pgtcl commands installed
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.4 1996/11/11 13:41:25 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global
include ../Makefile.global

CFLAGS+=  -I$(TCL_INCDIR) -I$(TK_INCDIR) -I$(X11_INCDIR) -I$(SRCDIR)/libpgtcl

# try to find libpgtcl.a in either directory
LIBPGTCL= -L$(SRCDIR)/libpgtcl -L$(LIBDIR) -lpgtcl

all: pgtclsh pgtksh

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) -o $@ pgtclAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) -L$(TCL_LIBDIR) $(TCL_LIB) -lm $(LD_ADD)

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) -o $@ pgtkAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) -L$(TCL_LIBDIR) -L$(TK_LIBDIR) -L$(X11_LIBDIR) \
	  $(TK_LIB) $(TCL_LIB) -lX11 -lm $(LD_ADD)

install: pgtclsh pgtksh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtclsh $(DESTDIR)$(BINDIR)/pgtclsh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtksh $(DESTDIR)$(BINDIR)/pgtksh

clean:
	rm -f pgtclAppInit.o pgtkAppInit.o pgtclsh pgtksh
