#-------------------------------------------------------------------------
#
# Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/libpgtcl/Makefile,v 1.36 2004/02/10 07:26:25 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpgtcl
top_builddir = ../../..
include ../../Makefile.global

NAME= pgtcl
SO_MAJOR_VERSION= 2
SO_MINOR_VERSION= 5

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) $(TCL_INCLUDE_SPEC)

OBJS= pgtcl.o pgtclCmds.o pgtclId.o

SHLIB_LINK = $(libpq) $(TCL_LIB_SPEC) $(TCL_LIBS) \
	$(filter -lintl -lssl -lcrypto -lkrb5 -lcrypt, $(LIBS)) $(THREAD_LIBS)

all: submake-libpq all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-headers install-lib

.PHONY: install-headers
install-headers: libpgtcl.h
	$(INSTALL_DATA) $< $(DESTDIR)$(includedir)/libpgtcl.h

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir)

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(includedir)/libpgtcl.h

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
