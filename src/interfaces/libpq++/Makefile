#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.6 1997/02/13 10:00:23 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global
SRCHEADERDIR = ../include
LIBPQHEADERDIR = $(SRCHEADERDIR)/libpq

LIBNAME= libpq++

# We have to override -Werror, which makes warnings, fatal, because we
# inevitably get the warning, "abstract declarator used as declaration"
# because of our inclusion of c.h and we don't know how to stop that.

CXXFLAGS= $(CFLAGS) -Wno-error

INCLUDE_OPT= \
             -I../backend \
             -I$(SRCHEADERDIR) \
             -I$(LIBPQDIR)

CXXFLAGS+= $(INCLUDE_OPT)
#CXXFLAGS+= -DDEBUG

ifdef KRBVERS
CXXFLAGS+= $(KRBFLAGS)
endif


OBJS = pgenv.o pgconnection.o pgtransdb.o pgcursordb.o pglobject.o

all: $(LIBNAME).a install examples

$(LIBNAME).a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) $(LIBNAME).a $(OBJS)
else
	$(AR) $(AROPT) $(LIBNAME).a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) $(LIBNAME).a

.PHONY: examples
examples:
	$(MAKE) -C examples all

.PHONY: beforeinstall-headers install-headers 
.PHONY: install install-$(LIBNAME) doc

install: install-headers install-$(LIBNAME) doc

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

install-$(LIBNAME): $(LIBNAME).a
	$(INSTALL) $(INSTL_LIB_OPTS) $(LIBNAME).a $(DESTDIR)$(LIBDIR)/$(LIBNAME).a

doc:
	$(MAKE) -C man install

clean:
	rm $(LIBNAME).a $(OBJS)
	$(MAKE) -C examples clean


###########################################################################
# Dependencies for the library
###########################################################################
include ./dependencies

