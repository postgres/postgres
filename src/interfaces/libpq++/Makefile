#-------------------------------------------------------------------------
#
# Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.26 2000/10/20 21:04:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq++
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

NAME= pq++
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 1

override CPPFLAGS += -I$(libpq_srcdir)

OBJS = pgconnection.o pgdatabase.o pgtransdb.o pgcursordb.o pglobject.o 

ifeq ($(PORTNAME), win)
SHLIB_LINK+= --driver-name g++ $(libpq)
else
SHLIB_LINK= $(libpq)
endif

# For CC on IRIX, must use CC as linker/archiver of C++ libraries
ifeq ($(PORTNAME), irix5)
  ifneq ($(GXX), yes)
    AR		:= CC
    AROPT	:= -ar -o
    LD		:= CC
  endif
endif
# Same for Solaris with native compiler
ifeq ($(PORTNAME), solaris)
  ifneq ($(GXX), yes)
    AR		:= CC
    AROPT	:= -xar -o
    LD		:= CC
  endif
endif


all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# Pull shared-lib CFLAGS into CXXFLAGS
override CXXFLAGS+= $(CFLAGS_SL)


.PHONY: examples
examples:
	$(MAKE) -C examples all

install: all installdirs install-headers install-lib

libpqxx_includedir = $(includedir)/libpq++
MAINHEADER = libpq++.h
LIBPGXXHEADERS = pgconnection.h pgdatabase.h pgtransdb.h pgcursordb.h pglobject.h

.PHONY: install-headers
install-headers: $(MAINHEADER) $(LIBPGXXHEADERS)
	$(INSTALL_DATA) $< $(DESTDIR)$(includedir)
	for i in $(LIBPGXXHEADERS); do $(INSTALL_DATA) $(srcdir)/$$i $(DESTDIR)$(libpqxx_includedir) || exit 1; done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) $(DESTDIR)$(libpqxx_includedir)

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(includedir)/$(MAINHEADER)
	for i in $(LIBPGXXHEADERS); do rm -f $(DESTDIR)$(libpqxx_includedir)/$$i || exit; done

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	-$(MAKE) -C examples clean


dep depend:
	$(CXX) -MM $(CPPFLAGS) $(CXXFLAGS) *.cc >depend

ifeq (depend,$(wildcard depend))
include depend
endif
