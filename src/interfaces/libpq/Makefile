#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.10 1996/11/03 07:14:29 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

INCLUDE_OPT= -I../include -I.

CFLAGS+= $(INCLUDE_OPT) -DPOSTPORT='"$(POSTPORT)"'

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

# dllist.c is found in backend/lib
VPATH:= $(VPATH):../backend/lib

OBJS= fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-lobj.o \
         ../backend/lib/dllist.o pqsignal.o 
ifeq ($(PORTNAME), next)
VPATH:=$(VPATH):../backend/port/$(PORTNAME)
OBJS+= getcwd.o putenv.o
endif

ifdef LINUX_ELF
shlib := libpq.so.1
else
shlib := 
endif

all: libpq.a $(shlib) postgres.h c.h

libpq.a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) libpq.a $(OBJS) 
else
	$(AR) $(AROPT) libpq.a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) libpq.a

libpq.so.1: $(OBJS)
	$(CC) $(LDFLAGS) -shared $(OBJS) -o libpq.so.1

# If dllist.o is out of date, we will not cause it to get remade.  We
# only make it if it doesn't exist.

../backend/lib/dllist.o:
	$(MAKE) -C ../backend/lib dllist.o

postgres.h: ../include/postgres.h
# Note: ../backend/include/postgres.h needs to be named something different
# to avoid confusion with this thing we're building now.
#
# hardwire NAMEDATALEN and OIDNAMELEN into the postgres.h for this installation
	rm -f postgres.h
	echo "#define NAMEDATALEN $(NAMEDATALEN)" >> postgres.h
	echo "#define OIDNAMELEN $(OIDNAMELEN)" >> postgres.h
	cat ../include/postgres.h >> postgres.h
	
c.h: ../include/c.h
	rm -f c.h
	echo "#undef PORTNAME" >  c.h
	echo "#define PORTNAME $(PORTNAME)" >> c.h
	echo "#undef PORTNAME_$(PORTNAME)" >>  c.h
	echo "#define PORTNAME_$(PORTNAME)" >> c.h
	cat ../include/c.h >> c.h

.PHONY: beforeinstall-headers install-headers
.PHONY: install install-libpq install-shlib-dep 

install: install-headers install-libpq $(install-shlib-dep)

install-headers: beforeinstall-headers postgres.h c.h \
                 ../include/config.h ../include/libpq/pqcomm.h \
                 ../include/libpq/libpq-fs.h ../include/lib/dllist.h \
                 ../include/utils/geo-decls.h libpq.fe.h
	$(INSTALL) $(INSTLOPTS) ../include/config.h \
          $(HEADERDIR)/config.h
	$(INSTALL) $(INSTLOPTS) ../include/libpq/pqcomm.h \
          $(HEADERDIR)/libpq/pqcomm.h
	$(INSTALL) $(INSTLOPTS) ../include/libpq/libpq-fs.h \
          $(HEADERDIR)/libpq/libpq-fs.h
	$(INSTALL) $(INSTLOPTS) ../include/lib/dllist.h \
          $(HEADERDIR)/lib/dllist.h
	$(INSTALL) $(INSTLOPTS) ../include/utils/geo-decls.h \
          $(HEADERDIR)/utils/geo-decls.h
	$(INSTALL) $(INSTLOPTS) libpq-fe.h $(HEADERDIR)/libpq-fe.h
ifeq ($(PORTNAME), hpux)
	$(INSTALL) $(INSTLOPTS) ../backend/port/hpux/fixade.h \
          $(HEADERDIR)/port/hpux/fixade.h
endif
	$(INSTALL) $(INSTLOPTS) c.h $(HEADERDIR)/c.h
	$(INSTALL) $(INSTLOPTS) postgres.h $(HEADERDIR)/postgres.h

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

ifdef LINUX_ELF
install-shlib-dep := install-shlib
else
install-shlib-dep :=
endif

install-libpq:
	$(INSTALL) $(INSTL_LIB_OPTS) libpq.a $(DESTDIR)$(LIBDIR)/libpq.a

install-shlib:
	$(INSTALL) $(INSTL_LIB_OPTS) libpq.so.1 $(DESTDIR)$(LIBDIR)/libpq.so.1

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

.PHONY: clean
clean:
	rm -f libpq.a libpq.so.1 $(OBJS) c.h postgres.h

ifeq (depend,$(wildcard depend))
include depend
endif
