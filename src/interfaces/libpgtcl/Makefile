#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.8 1997/01/10 18:55:14 momjian Exp $
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

ifeq ($(PORTNAME), linux)
  ifdef LINUX_ELF
    ifeq ($(CC), gcc)
      CFLAGS += -fpic -fPIC
    endif
  endif
endif

OBJS= pgtcl.o pgtclCmds.o pgtclId.o

ifdef LINUX_ELF
  shlib             := libpgtcl.so.1
  install-shlib-dep := install-shlib
  LDFLAGS           += -L ../libpq -lpq
else
  shlib             := 
  install-shlib-dep :=
endif

all: libpgtcl.a $(shlib)

libpgtcl.a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) libpgtcl.a $(OBJS)
else
	$(AR) $(AROPT) libpgtcl.a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) libpgtcl.a

libpgtcl.so.1: $(OBJS)
	$(CC) $(LDFLAGS) -shared $(OBJS) -o libpgtcl.so.1
	rm -f libpgtcl.so
	ln -s libpgtcl.so.1 libpgtcl.so

.PHONY: beforeinstall-headers install-headers
.PHONY: install install-libpgtcl

install: install-headers install-libpgtcl $(install-shlib-dep)

install-headers: beforeinstall-headers libpgtcl.h
	$(INSTALL) $(INSTLOPTS) libpgtcl.h $(HEADERDIR)/libpgtcl.h

beforeinstall-headers:
	@if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi

install-libpgtcl: libpgtcl.a
	$(INSTALL) $(INSTL_LIB_OPTS) libpgtcl.a $(DESTDIR)$(LIBDIR)/libpgtcl.a

install-shlib: libpgtcl.so.1
	$(INSTALL) $(INSTL_LIB_OPTS) libpgtcl.so.1 \
		$(DESTDIR)$(LIBDIR)/libpgtcl.so
	rm -f $(DESTDIR)$(LIBDIR)/libpgtcl.so
	ln -s libpgtcl.so.1 $(DESTDIR)$(LIBDIR)/libpgtcl.so

.PHONY: clean
clean: 
	rm -f $(OBJS)
