#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.5 1996/09/23 08:23:33 scrappy Exp $
#
#-------------------------------------------------------------------------

MKDIR=	../mk
include $(MKDIR)/postgres.mk

CFLAGS+= -I../backend/include \
	-I$../include \
	-I$../backend \
	-I. \

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

# dllist.c is found in backend/lib
VPATH:= $(VPATH):../backend/lib

LIBSRCS= fe-auth.c fe-connect.c fe-exec.c fe-misc.c fe-lobj.c \
	dllist.c pqsignal.c 
ifeq ($(PORTNAME), next)
VPATH:=$(VPATH):../backend/port/$(PORTNAME)
LIBSRCS+= getcwd.c putenv.c
endif

all::   libpq.a postgres.h c.h

postgres.h: ../backend/include/postgres.h
# Note: ../backend/include/postgres.h needs to be named something different
# to avoid confusion with this thing we're building now.
#
# hardwire NAMEDATALEN and OIDNAMELEN into the postgres.h for this installation
	rm -f obj/postgres.h
	echo "#define NAMEDATALEN $(NAMEDATALEN)" >> obj/postgres.h
	echo "#define OIDNAMELEN $(OIDNAMELEN)" >> obj/postgres.h
	cat ../backend/include/postgres.h >> obj/postgres.h
	
c.h: ../backend/include/c.h
	rm -f obj/c.h
	echo "#undef PORTNAME" >  obj/c.h
	echo "#define PORTNAME $(PORTNAME)" >> obj/c.h
	echo "#undef PORTNAME_$(PORTNAME)" >>  obj/c.h
	echo "#define PORTNAME_$(PORTNAME)" >> obj/c.h
	cat ../backend/include/c.h >> obj/c.h

.PHONY: beforeinstall-headers install-headers

ifndef NO_BEFOREINSTL
beforeinstall-headers:
	@if [ ! -d $(HEADERDIR) ]; then mkdir $(HEADERDIR); fi
	@if [ ! -d $(HEADERDIR)/port ]; then mkdir $(HEADERDIR)/port; fi
	@if [ ! -d $(HEADERDIR)/port/$(PORTNAME) ]; \
		then mkdir $(HEADERDIR)/port/$(PORTNAME); fi
	@if [ ! -d $(HEADERDIR)/include ]; \
		then mkdir $(HEADERDIR)/include; fi
	@if [ ! -d $(HEADERDIR)/lib ]; \
		then mkdir $(HEADERDIR)/lib; fi
	@if [ ! -d $(HEADERDIR)/libpq ]; \
		then mkdir $(HEADERDIR)/libpq; fi
	@if [ ! -d $(HEADERDIR)/utils ]; \
		then mkdir $(HEADERDIR)/utils; fi
else
beforeinstall-headers: .dosomething
endif

TEMPDIR=obj

install-headers: beforeinstall-headers postgres.h c.h
	$(INSTALL) $(INSTLOPTS) ../backend/include/postgres.h \
          $(HEADERDIR)/postgres.h
	$(INSTALL) $(INSTLOPTS) ../include/libpq/pqcomm.h \
          $(HEADERDIR)/libpq/pqcomm.h
	$(INSTALL) $(INSTLOPTS) ../include/libpq/libpq-fs.h \
          $(HEADERDIR)/libpq/libpq-fs.h
	$(INSTALL) $(INSTLOPTS) ../include/lib/dllist.h \
          $(HEADERDIR)/libpq/dllist.h
	$(INSTALL) $(INSTLOPTS) ../include/utils/geo-decls.h \
          $(HEADERDIR)/utils/geo-decls.h
	$(INSTALL) $(INSTLOPTS) libpq-fe.h $(HEADERDIR)/libpq-fe.h
ifeq ($(PORTNAME), hpux)
	$(INSTALL) $(INSTLOPTS) ../backend/port/hpux/fixade.h \
          $(HEADERDIR)/port/hpux/fixade.h
endif
	$(INSTALL) $(INSTLOPTS) obj/c.h $(HEADERDIR)/c.h
	$(INSTALL) $(INSTLOPTS) obj/postgres.h $(HEADERDIR)/postgres.h

install:: install-headers

CLEANFILES+= c.h postgres.h

LIB= pq
include $(MKDIR)/postgres.lib.mk


