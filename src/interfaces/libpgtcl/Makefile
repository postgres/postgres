#-------------------------------------------------------------------------
#
# Makefile for libpgtcl library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/Makefile,v 1.20 2000/08/31 16:11:45 petere Exp $
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

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-headers install-lib

.PHONY: install-headers
install-headers: libpgtcl.h
	$(INSTALL_DATA) $< $(includedir)/libpgtcl.h

installdirs:
	$(mkinstalldirs) $(libdir) $(includedir)

uninstall: uninstall-lib
	rm -f $(includedir)/libpgtcl.h

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
