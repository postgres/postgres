#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpgeasy
#
# $Header: /cvsroot/pgsql/src/interfaces/libpgeasy/Attic/Makefile,v 1.3 2000/09/17 13:02:48 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpgeasy
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# shared library parameters
NAME= pgeasy
SO_MAJOR_VERSION= 2
SO_MINOR_VERSION= 1

CPPFLAGS += -I$(libpq_srcdir)

OBJS= libpgeasy.o halt.o

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
install-headers: libpgeasy.h
	$(INSTALL_DATA) $< $(DESTDIR)$(includedir)/libpgeasy.h

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir)

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(includedir)/libpgeasy.h

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
