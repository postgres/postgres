#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.1.1.1 1996/07/09 06:22:18 scrappy Exp $
#
#-------------------------------------------------------------------------

CPP_LIB=	true

LIB=	pq++

MKDIR=	../mk
include $(MKDIR)/postgres.mk

CXXFLAGS = $(CFLAGS)

CXXFLAGS+= -I$(srcdir)/backend/include \
	-I$(srcdir)/backend \
	-I$(srcdir)/libpq \
	-I$(CURDIR) \

ifdef KRBVERS
CXXFLAGS+= $(KRBFLAGS)
endif


LIBSRCS =  pgenv.cc pgconnection.cc pglobject.cc

.PHONY: beforeinstall-headers install-headers

ifndef NO_BEFOREINSTL
beforeinstall-headers:
	@-if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi
else
beforeinstall-headers: .dosomething
endif

HEADERFILES = libpq++.H

install-headers: beforeinstall-headers
	@for i in ${HEADERFILES}; do \
		echo "Installing $(HEADERDIR)/$$i."; \
		$(INSTALL) -c -m 444 $$i $(HEADERDIR)/$$i; \
	done

install:: install-headers

include $(MKDIR)/postgres.lib.mk
