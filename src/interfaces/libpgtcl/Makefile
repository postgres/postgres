#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.1.1.1 1996/07/09 06:22:16 scrappy Exp $
#
#-------------------------------------------------------------------------

LIB=	pgtcl

MKDIR=	../mk
include $(MKDIR)/postgres.mk

CFLAGS+= -I$(HEADERDIR) \
	 -I$(srcdir)/backend/include \
	 -I$(srcdir)/backend \
	 -I$(CURDIR) \
	 -I$(TCL_INCDIR)

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

LIBSRCS= pgtcl.c pgtclCmds.c pgtclId.c

install-headers:
	$(INSTALL) $(INSTLOPTS) libpgtcl.h $(HEADERDIR)/libpgtcl.h


install:: install-headers

include $(MKDIR)/postgres.lib.mk

