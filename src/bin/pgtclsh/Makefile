#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for a tclsh workalike with pgtcl commands installed
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.8 1997/09/27 19:13:56 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

CFLAGS+=  -I$(TCL_INCDIR) -I$(TK_INCDIR) -I$(X11_INCDIR) -I$(SRCDIR)/interfaces/libpgtcl

# try to find libpgtcl.a in either directory
LIBPGTCL= -L$(SRCDIR)/libpgtcl -L$(LIBDIR) -lpgtcl
LIBPQ= -L$(LIBPQDIR) -L$(LIBDIR) -lpq

all: pgtclsh pgtksh

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) -o $@ pgtclAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) -L$(TCL_LIBDIR) $(TCL_LIB) -lm $(LDFLAGS)

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) -o $@ pgtkAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) -L$(TCL_LIBDIR) -L$(TK_LIBDIR) -L$(X11_LIBDIR) \
	  $(TK_LIB) $(TCL_LIB) -lX11 -lm $(LDFLAGS)

install: pgtclsh pgtksh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtclsh $(DESTDIR)$(BINDIR)/pgtclsh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtksh $(DESTDIR)$(BINDIR)/pgtksh

clean:
	rm -f pgtclAppInit.o pgtkAppInit.o pgtclsh pgtksh
