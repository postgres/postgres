#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.16 1997/09/27 19:35:04 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include $(SRCDIR)/Makefile.global

INCLUDE_OPT= \
             -I$(SRCDIR)/backend \
             -I$(SRCDIR)/include \
             -I$(LIBPQDIR) \
             -I$(TCL_INCDIR)

CFLAGS+= $(INCLUDE_OPT)
ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

shlib             := 
install-shlib-dep :=

ifeq ($(PORTNAME), linux)
  ifdef LINUX_ELF
    ifeq ($(CC), gcc)
      CFLAGS += -fpic -fPIC
    endif
    shlib             := libpgtcl.so.1
    install-shlib-dep := install-shlib
    LDFLAGS           += -L $(SRCDIR)/interfaces/libpq -lpq
  endif
endif

ifeq ($(PORTNAME), i386_solaris)
  CFLAGS+= -fPIC
endif

OBJS= pgtcl.o pgtclCmds.o pgtclId.o


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
		$(DESTDIR)$(LIBDIR)/libpgtcl.so.1
	rm -f $(DESTDIR)$(LIBDIR)/libpgtcl.so
	ln -s libpgtcl.so.1 $(DESTDIR)$(LIBDIR)/libpgtcl.so

.PHONY: clean
clean: 
	rm -f $(OBJS)
