#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.5 1996/11/18 01:43:54 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

# We have to override -Werror, which makes warnings, fatal, because we
# inevitably get the warning, "abstract declarator used as declaration"
# because of our inclusion of c.h and we don't know how to stop that.

CXXFLAGS= $(CFLAGS) -Wno-error

INCLUDE_OPT= \
             -I../backend \
             -I../include \
             -I$(LIBPQDIR) 

CXXFLAGS+= $(INCLUDE_OPT)

ifdef KRBVERS
CXXFLAGS+= $(KRBFLAGS)
endif

OBJS= pgenv.o pgconnection.o pglobject.o

all: libpq++.a examples

libpq++.a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) libpq++.a $(OBJS)
else
	$(AR) $(AROPT) libpq++.a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) libpq++.a

.PHONY: examples
examples:
	$(MAKE) -C examples all

.PHONY: beforeinstall-headers install-headers 
.PHONY: install install-libpq++ doc

install: install-headers install-libpq++ doc

install-headers: beforeinstall-headers libpq++.H
	$(INSTALL) $(INSTLOPTS) libpq++.H $(HEADERDIR)/libpq++.H

beforeinstall-headers:
	@if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi

install-libpq++: libpq++.a
	$(INSTALL) $(INSTL_LIB_OPTS) libpq++.a $(DESTDIR)$(LIBDIR)/libpq++.a

doc:
	$(MAKE) -C man install

clean:
	rm libpq++.a $(OBJS)
	$(MAKE) -C examples clean
