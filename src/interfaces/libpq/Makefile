#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.26 1997/01/10 18:55:24 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

# We need the backend directory here for its fmgr.h
INCLUDE_OPT= -I../include -I../backend

CFLAGS+= $(INCLUDE_OPT) 

ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
endif

# dllist.c is found in backend/lib
VPATH:= $(VPATH):../backend/lib
 
OBJS= fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-lobj.o \
         ../backend/lib/dllist.o pqsignal.o 


shlib := 
install-shlib-dep :=
ifeq ($(PORTNAME), linux)
  ifdef LINUX_ELF
    install-shlib-dep := install-shlib
    shlib := libpq.so.1
  endif
endif

all: libpq.a $(shlib) c.h

libpq.a: $(OBJS)
ifdef MK_NO_LORDER
	$(AR) $(AROPT) libpq.a $(OBJS) 
else
	$(AR) $(AROPT) libpq.a `lorder $(OBJS) | tsort`
endif
	$(RANLIB) libpq.a

fe-lobj.o: ../backend/fmgr.h

# The following rules cause dependencies in the backend directory to 
# get made if they don't exist, but don't cause them to get remade if they
# are out of date.
../backend/fmgr.h:
	$(MAKE) -C ../backend fmgr.h

../backend/lib/dllist.o:
	$(MAKE) -C ../backend/lib dllist.o

$(shlib): $(OBJS)
	$(LD) $(LDFLAGS) -shared $(OBJS) -o $(shlib)

c.h: ../include/c.h
	rm -f c.h
	echo "#undef PORTNAME" >  c.h
	echo "#define PORTNAME $(PORTNAME)" >> c.h
	cat ../include/c.h >> c.h

.PHONY: beforeinstall-headers install-headers
.PHONY: install install-libpq install-shlib-dep 

install: install-headers install-libpq $(install-shlib-dep)

# Many of the headers we install below have nothing to do with libpq,
# so should be installed by someone else.
#
install-headers: beforeinstall-headers c.h \
                 ../include/postgres.h ../include/postgres_ext.h \
                 ../include/config.h ../include/libpq/pqcomm.h \
                 ../include/libpq/libpq-fs.h ../include/lib/dllist.h \
                 ../include/utils/geo-decls.h libpq-fe.h
	$(INSTALL) $(INSTLOPTS) ../include/config.h \
          $(HEADERDIR)/config.h
	$(INSTALL) $(INSTLOPTS) ../include/postgres.h \
          $(HEADERDIR)/postgres.h
	$(INSTALL) $(INSTLOPTS) ../include/postgres_ext.h \
          $(HEADERDIR)/postgres_ext.h
	$(INSTALL) $(INSTLOPTS) ../include/libpq/pqcomm.h \
          $(HEADERDIR)/libpq/pqcomm.h
	$(INSTALL) $(INSTLOPTS) ../include/libpq/libpq-fs.h \
          $(HEADERDIR)/libpq/libpq-fs.h
	$(INSTALL) $(INSTLOPTS) ../include/lib/dllist.h \
          $(HEADERDIR)/lib/dllist.h
	$(INSTALL) $(INSTLOPTS) ../include/utils/geo-decls.h \
          $(HEADERDIR)/utils/geo-decls.h
	$(INSTALL) $(INSTLOPTS) ../include/utils/elog.h \
          $(HEADERDIR)/utils/elog.h
	$(INSTALL) $(INSTLOPTS) ../include/utils/palloc.h \
          $(HEADERDIR)/utils/palloc.h
	$(INSTALL) $(INSTLOPTS) ../include/access/attnum.h \
          $(HEADERDIR)/access/attnum.h
	$(INSTALL) $(INSTLOPTS) libpq-fe.h $(HEADERDIR)/libpq-fe.h
ifeq ($(PORTNAME), hpux)
	$(INSTALL) $(INSTLOPTS) ../backend/port/hpux/fixade.h \
          $(HEADERDIR)/port/hpux/fixade.h
endif
	$(INSTALL) $(INSTLOPTS) c.h $(HEADERDIR)/c.h

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
	@if [ ! -d $(HEADERDIR)/access ]; \
		then mkdir $(HEADERDIR)/access; fi

install-libpq: libpq.a
	$(INSTALL) $(INSTL_LIB_OPTS) libpq.a $(DESTDIR)$(LIBDIR)/libpq.a

install-shlib: $(shlib)
	$(INSTALL) $(INSTL_LIB_OPTS) $(shlib) $(DESTDIR)$(LIBDIR)/$(shlib)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

.PHONY: clean
clean:
	rm -f libpq.a $(shlib) $(OBJS) c.h

ifeq (depend,$(wildcard depend))
include depend
endif
