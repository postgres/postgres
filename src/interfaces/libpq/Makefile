#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.45 2000/10/20 21:04:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 2
SO_MINOR_VERSION= 1

override CPPFLAGS += -DFRONTEND -I$(srcdir) -DSYSCONFDIR='"$(sysconfdir)"'

OBJS= fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-print.o fe-lobj.o \
      pqexpbuffer.o dllist.o pqsignal.o $(SNPRINTF) $(INET_ATON)

ifdef MULTIBYTE
OBJS+= common.o wchar.o conv.o big5.o mbutils.o
endif

# If crypt is a separate library, rather than part of libc,
# make sure it gets included in shared libpq.
SHLIB_LINK+= $(findstring -lcrypt, $(LIBS))

# Include kerberos libraries into libpq
SHLIB_LINK += $(KRB_LIBS)

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib


# We use several backend modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.

backend_src = $(top_srcdir)/src/backend

dllist.c: $(backend_src)/lib/dllist.c
	rm -f $@ && $(LN_S) $< .

# this only gets done if configure finds system doesn't have snprintf()
snprintf.c: $(backend_src)/port/snprintf.c
	rm -f $@ && $(LN_S) $< .

# this only gets done if configure finds system doesn't have inet_aton()
inet_aton.c: $(backend_src)/port/inet_aton.c
	rm -f $@ && $(LN_S) $< .

ifdef MULTIBYTE
maps = iso8859.map UTF_to_EUC_JP.map EUC_JP_to_UTF.map sjis.map
conv.c: $(maps)
common.c wchar.c conv.c big5.c mbutils.c $(maps) : % : $(backend_src)/utils/mb/%
	rm -f $@ && $(LN_S) $< .
endif


install: all installdirs install-headers install-lib

.PHONY: install-headers
install-headers: libpq-fe.h libpq-int.h pqexpbuffer.h
	$(INSTALL_DATA) $(srcdir)/libpq-fe.h $(DESTDIR)$(includedir)
	$(INSTALL_DATA) $(srcdir)/libpq-int.h $(DESTDIR)$(includedir)
	$(INSTALL_DATA) $(srcdir)/pqexpbuffer.h $(DESTDIR)$(includedir)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir)

uninstall: uninstall-lib
	rm -f $(addprefix $(DESTDIR)$(includedir)/, libpq-fe.h libpq-int.h pqexpbuffer.h)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) dllist.c snprintf.c inet_aton.c common.c wchar.c conv.c big5.c

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
