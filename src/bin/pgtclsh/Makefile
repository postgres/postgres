#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for a tclsh workalike with pgtcl commands installed
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.18 1998/05/12 15:42:08 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

CFLAGS+=  $(X_CFLAGS) -I$(SRCDIR)/interfaces/libpgtcl

ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

# try to find libpgtcl.a in either directory
LIBPGTCL= -L$(SRCDIR)/interfaces/libpgtcl -lpgtcl
LIBPQ= -L$(LIBPQDIR) -lpq

all: pgtclsh pgtksh

pgtclsh: pgtclAppInit.o
	$(CC) $(CFLAGS) -o $@ pgtclAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) $(TCL_LIB) -lm $(LDFLAGS)

pgtksh: pgtkAppInit.o
	$(CC) $(CFLAGS) -o $@ pgtkAppInit.o \
	  $(LIBPGTCL) $(LIBPQ) $(X_LIBS) $(TK_LIB) $(TCL_LIB) \
	  $(X11_LIBS) -lm $(LDFLAGS)

install: pgtclsh pgtksh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtclsh $(BINDIR)/pgtclsh
	$(INSTALL) $(INSTL_EXE_OPTS) pgtksh $(BINDIR)/pgtksh

clean:
	rm -f pgtclAppInit.o pgtkAppInit.o pgtclsh pgtksh

dep depend:
	$(CC) -MM $(CFLAGS) *.c > depend
