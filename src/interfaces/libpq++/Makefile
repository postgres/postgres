#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.13 1998/04/06 01:16:47 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include $(SRCDIR)/Makefile.global
SRCHEADERDIR = $(SRCDIR)/include
LIBPQHEADERDIR = $(SRCHEADERDIR)/libpq

LIBNAME= libpq++

# We have to override -Werror, which makes warnings, fatal, because we
# inevitably get the warning, "abstract declarator used as declaration"
# because of our inclusion of c.h and we don't know how to stop that.

CXXFLAGS= $(CFLAGS) -Wno-error

INCLUDE_OPT= \
             -I$(SRCDIR)/backend \
             -I$(SRCHEADERDIR) \
             -I$(LIBPQDIR)

CXXFLAGS+= $(INCLUDE_OPT)
#CXXFLAGS+= -DDEBUG

ifdef KRBVERS
CXXFLAGS+= $(KRBFLAGS)
endif


OBJS = pgenv.o pgconnection.o pgtransdb.o pgcursordb.o pglobject.o

# Shared library stuff
SHLIB := 
INSTALL-SHLIB-DEP :=
ifeq ($(PORTNAME), i386_solaris)
  INSTALL-SHLIB-DEP := install-shlib
  SHLIB := libpq++.so.1
  LDFLAGS_SL = -G -z text
  CFLAGS += $(CFLAGS_SL)
endif
ifeq ($(PORTNAME), hpux)
  install-shlib-dep := install-shlib
  shlib := libpq.sl
  LDFLAGS_SL = -b
  CFLAGS += $(CFLAGS_SL)
endif

all: $(LIBNAME).a $(SHLIB) install examples

$(LIBNAME).a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) $(LIBNAME).a $(OBJS)
else
	$(AR) $(AROPT) $(LIBNAME).a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) $(LIBNAME).a

$(SHLIB): $(OBJS)
	$(LD) $(LDFLAGS) $(LDFLAGS_SL) -o $@ $(OBJS)

.PHONY: examples
examples:
	$(MAKE) -C examples all

.PHONY: beforeinstall-headers install-headers 
.PHONY: install beforeinstall-lib install-$(LIBNAME) doc

install: install-headers install-$(LIBNAME) $(INSTALL-SHLIB-DEP) doc

LIBPGXXDIR = $(LIBNAME)
LIBPGXXHEADERDIR = $(HEADERDIR)/$(LIBPGXXDIR)
MAINHEADER = $(LIBNAME).h
LIBPGXXHEADERS = pgenv.h \
		 pgconnection.h \
		 pgdatabase.h \
		 pgtransdb.h \
		 pgcursordb.h \
		 pglobject.h

install-headers: beforeinstall-headers $(MAINHEADER)
	@$(INSTALL) $(INSTLOPTS) $(MAINHEADER) $(HEADERDIR)/$(MAINHEADER)
	@for i in ${LIBPGXXHEADERS}; do \
		echo "Installing $(LIBPGXXHEADERDIR)/$$i."; \
		$(INSTALL) $(INSTLOPTS) $$i $(LIBPGXXHEADERDIR)/$$i; \
	done

beforeinstall-headers:
	@if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi
	@if [ ! -d $(LIBPGXXHEADERDIR) ]; then mkdir $(LIBPGXXHEADERDIR); fi

beforeinstall-lib:
	@if [ ! -d $(DESTDIR)/$(LIBDIR) ] ; then mkdir $(DESTDIR)/$(LIBDIR); fi

install-$(LIBNAME): $(LIBNAME).a beforeinstall-lib
	$(INSTALL) $(INSTL_LIB_OPTS) $(LIBNAME).a $(DESTDIR)$(LIBDIR)/$(LIBNAME).a

install-shlib: $(SHLIBNAME) beforeinstall-lib
	$(INSTALL) $(INSTL_SHLIB_OPTS) $(SHLIB) $(DESTDIR)/$(LIBDIR)/$(SHLIB)
	rm -f $(DESTDIR)/$(LIBDIR)/libpq++.so
	ln -sf $(SHLIB) $(DESTDIR)/$(LIBDIR)/libpq++.so


doc:
	$(MAKE) -C man install

clean:
	rm $(LIBNAME).a $(OBJS)
	$(MAKE) -C examples clean

dep depend:
	$(CXX) -MM $(CXXFLAGS) *.cc > depend

ifeq (depend,$(wildcard depend))
include depend
endif
