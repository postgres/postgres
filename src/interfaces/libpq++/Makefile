#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.19 1998/07/12 19:20:45 momjian Exp $
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
ifeq ($(PORTNAME), linux)
  INSTALL-SHLIB-DEP := install-shlib
  SHLIB := libpq++.so.1
  LDFLAGS_SL = -shared -soname $(SHLIB)
  CFLAGS += $(CFLAGS_SL)
endif

ifeq ($(PORTNAME), solaris)
  INSTALL-SHLIB-DEP := install-shlib
  SHLIB := libpq++.so.1
  LDFLAGS_SL = -G -z text -shared -soname $(SHLIB)
  CFLAGS += $(CFLAGS_SL)
endif

ifeq ($(PORTNAME), hpux)
  install-shlib-dep := install-shlib
  shlib := libpq.sl
  LDFLAGS_SL = -b
  CFLAGS += $(CFLAGS_SL)
endif

all: $(LIBNAME).a $(SHLIB)

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
	@if [ ! -d $(LIBDIR) ] ; then mkdir $(LIBDIR); fi

install-$(LIBNAME): $(LIBNAME).a beforeinstall-lib
	$(INSTALL) $(INSTL_LIB_OPTS) $(LIBNAME).a $(LIBDIR)/$(LIBNAME).a

install-shlib: $(SHLIBNAME) beforeinstall-lib
	$(INSTALL) $(INSTL_SHLIB_OPTS) $(SHLIB) $(LIBDIR)/$(SHLIB)
	rm -f $(LIBDIR)/libpq++.so
	ln -sf $(SHLIB) $(LIBDIR)/libpq++.so


doc:
	$(MAKE) -C man install

clean:
	rm -f $(LIBNAME).a $(OBJS)
	$(MAKE) -C examples clean

dep depend:
	$(CXX) -MM $(CXXFLAGS) *.cc > depend

ifeq (depend,$(wildcard depend))
include depend
endif
