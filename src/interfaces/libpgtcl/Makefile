#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.4 1996/11/08 00:58:07 scrappy Exp $
#
#-------------------------------------------------------------------------

LIB=	pgtcl

MKDIR=	../mk
include $(MKDIR)/postgres.mk

CFLAGS+= -I$(HEADERDIR) \
	 -I$(srcdir)/backend/include \
	 -I$(srcdir)/backend \
	 -I$(CURDIR) \
	 -I$(TCL_INCDIR) \
	 -I$(srcdir)/libpq

LIBLDLIBS+= -L$(LIBDIR) -lpq

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

ifdef LINUX_ELF
CFLAGS += -fPIC
CC += -L $(POSTGRESDIR)/lib -lpq
endif

LIBSRCS= pgtcl.c pgtclCmds.c pgtclId.c

install-headers:
	$(INSTALL) $(INSTLOPTS) libpgtcl.h $(HEADERDIR)/libpgtcl.h


install:: install-headers

include $(MKDIR)/postgres.lib.mk

