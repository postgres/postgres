#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.6 1996/11/13 10:35:31 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

INCLUDE_OPT= \
             -I../backend \
             -I../include \
             -I$(LIBPQDIR) \
             -I$(TCL_INCDIR)

CFLAGS+= $(INCLUDE_OPT)
ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

ifdef LINUX_ELF
CFLAGS += -fPIC
endif

OBJS= pgtcl.o pgtclCmds.o pgtclId.o

all: libpgtcl.a

libpgtcl.a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) libpgtcl.a $(OBJS)
else
	$(AR) $(AROPT) libpgtcl.a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) libpgtcl.a

.PHONY: beforeinstall-headers install-headers
.PHONY: install install-libpgtcl

install: install-headers install-libpgtcl

install-headers: beforeinstall-headers libpgtcl.h
	$(INSTALL) $(INSTLOPTS) libpgtcl.h $(HEADERDIR)/libpgtcl.h

beforeinstall-headers:
	@if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi

install-libpgtcl: libpgtcl.a
	$(INSTALL) $(INSTL_LIB_OPTS) libpgtcl.a $(DESTDIR)$(LIBDIR)/libpgtcl.a

.PHONY: clean
clean: 
	rm -f $(OBJS)
