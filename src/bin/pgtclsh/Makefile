#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for a tclsh workalike with pgtcl commands installed
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgtclsh/Attic/Makefile,v 1.2 1996/07/23 03:03:36 scrappy Exp $
#
#-------------------------------------------------------------------------

MKDIR=	../../mk
include $(MKDIR)/postgres.mk
include ../Makefile.global

CFLAGS+=  -I$(TCL_INCDIR) -I$(TK_INCDIR) -I$(X11_INCDIR) -I$(srcdir)/libpgtcl

# try to find libpgtcl.a in either directory
LIBPGTCL= -L$(srcdir)/libpgtcl/$(objdir) -L$(LIBDIR) -lpgtcl

pgtclsh: $(objdir)/pgtclAppInit.o
	$(CC) $(CDEBUG) -o $(objdir)/$(@F) $(objdir)/pgtclAppInit.o\
	$(LIBPGTCL) $(LIBPQ) -L$(TCL_LIBDIR) $(TCL_LIB) -lm $(LD_ADD)

pgtksh: $(objdir)/pgtkAppInit.o
	$(CC) $(CDEBUG) -o $(objdir)/$(@F) $(objdir)/pgtkAppInit.o \
	$(LIBPGTCL) $(LIBPQ) -L$(TCL_LIBDIR) -L$(TK_LIBDIR) -L$(X11_LIBDIR) \
	$(TK_LIB) $(TCL_LIB) -lX11 -lm $(LD_ADD)

install::	localobj pgtclsh pgtksh
	$(INSTALL) $(INSTL_EXE_OPTS) $(objdir)/pgtclsh $(DESTDIR)$(BINDIR)/pgtclsh
	$(INSTALL) $(INSTL_EXE_OPTS) $(objdir)/pgtksh $(DESTDIR)$(BINDIR)/pgtksh

CLEANFILES = pgtclAppInit.o pgtkAppInit.o pgtclsh pgtksh

PROG=pgtclsh

all:: pgtclsh pgtksh

# don't use the default template for generating executables since we have
# two executable targets
# include $(MKDIR)/postgres.prog.mk


