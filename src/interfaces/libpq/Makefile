#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.2 1996/07/29 20:52:24 scrappy Exp $
#
#-------------------------------------------------------------------------

LIB=	pq

MKDIR=	../mk
include $(MKDIR)/postgres.mk

CFLAGS+= -I$(srcdir)/backend/include \
	-I$(srcdir)/backend \
	-I$(CURDIR) \

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

# dllist.c is found in backend/lib
VPATH:= $(VPATH):$(srcdir)/backend/lib

LIBSRCS= fe-auth.c fe-connect.c fe-exec.c fe-misc.c fe-lobj.c \
	dllist.c pqsignal.c 
ifeq ($(PORTNAME), next)
VPATH:=$(VPATH):$(srcdir)/backend/port/$(PORTNAME)
LIBSRCS+= getcwd.c putenv.c
endif


.PHONY: beforeinstall-headers install-headers

ifndef NO_BEFOREINSTL
beforeinstall-headers:
	@-if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi
	@-if [ ! -d $(HEADERDIR)/port ]; then mkdir $(HEADERDIR)/port; fi
	@-if [ ! -d $(HEADERDIR)/port/$(PORTNAME) ]; \
		then mkdir $(HEADERDIR)/port/$(PORTNAME); fi
	@-if [ ! -d $(HEADERDIR)/include ]; \
		then mkdir $(HEADERDIR)/include; fi
	@-if [ ! -d $(HEADERDIR)/lib ]; \
		then mkdir $(HEADERDIR)/lib; fi
	@-if [ ! -d $(HEADERDIR)/libpq ]; \
		then mkdir $(HEADERDIR)/libpq; fi
	@-if [ ! -d $(HEADERDIR)/utils ]; \
		then mkdir $(HEADERDIR)/utils; fi
else
beforeinstall-headers: .dosomething
endif

HEADERFILES = include/postgres.h \
	      libpq/pqcomm.h \
	      libpq/libpq-fs.h \
	      lib/dllist.h \
	      utils/geo-decls.h

ifeq ($(PORTNAME), hpux)
HEADERFILES += port/hpux/fixade.h
endif


TEMPDIR=/tmp

install-headers: beforeinstall-headers
	@for i in ${HEADERFILES}; do \
		echo "Installing $(HEADERDIR)/$$i."; \
		$(INSTALL) $(INSTLOPTS) $(srcdir)/backend/$$i $(HEADERDIR)/$$i; \
	done
	$(INSTALL) $(INSTLOPTS) libpq-fe.h $(HEADERDIR)/libpq-fe.h
	@mv -f $(HEADERDIR)/include/* $(HEADERDIR)
	@rmdir $(HEADERDIR)/include
#	XXX - installing fmgr.h depends on the backend being built
	$(INSTALL) $(INSTLOPTS) $(srcdir)/backend/$(objdir)/fmgr.h $(HEADERDIR)/fmgr.h
	@rm -f $(TEMPDIR)/c.h
	@echo "#undef PORTNAME" >  $(TEMPDIR)/c.h
	@echo "#define PORTNAME $(PORTNAME)" >> $(TEMPDIR)/c.h
	@echo "#undef PORTNAME_$(PORTNAME)" >>  $(TEMPDIR)/c.h
	@echo "#define PORTNAME_$(PORTNAME)" >> $(TEMPDIR)/c.h
	@cat $(srcdir)/backend/include/c.h >> $(TEMPDIR)/c.h
	$(INSTALL) $(INSTLOPTS) $(TEMPDIR)/c.h $(HEADERDIR)/c.h
	@rm -f $(TEMPDIR)/postgres.h
# hardwire NAMEDATALEN and OIDNAMELEN into the postgres.h for this installation
	@echo "#define NAMEDATALEN $(NAMEDATALEN)" >> $(TEMPDIR)/postgres.h
	@echo "#define OIDNAMELEN $(OIDNAMELEN)" >> $(TEMPDIR)/postgres.h
	@cat $(srcdir)/backend/include/postgres.h >> $(TEMPDIR)/postgres.h
	$(INSTALL) $(INSTLOPTS) $(TEMPDIR)/postgres.h $(HEADERDIR)/postgres.h
	@rm -f $(TEMPDIR)/c.h $(TEMPDIR)/postgres.h

install:: install-headers

include $(MKDIR)/postgres.lib.mk

