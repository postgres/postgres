#-------------------------------------------------------------------------
#
# Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.22 2000/09/25 22:23:00 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpgtcl
top_builddir = ../../..
include ../../Makefile.global

NAME= pgtcl
SO_MAJOR_VERSION= 2
SO_MINOR_VERSION= 1

CPPFLAGS += -I$(libpq_srcdir)

OBJS= pgtcl.o pgtclCmds.o pgtclId.o

SHLIB_LINK+= $(libpq)

# If crypt is a separate library, rather than part of libc, it may need
# to be referenced separately to keep (broken) linkers happy.  (This is
# braindead; users of libpq should not need to know what it depends on.)
SHLIB_LINK+= $(findstring -lcrypt,$(LIBS))

all: submake all-lib

.PHONY: submake
submake:
	$(MAKE) -C $(libpq_builddir) all

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
