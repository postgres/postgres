#-------------------------------------------------------------------------
#
# Makefile for libpq++ library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/Makefile,v 1.23 2000/06/30 16:10:54 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq++
top_builddir = ../../..
include ../../Makefile.global

NAME= pq++
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 1

SRCHEADERDIR = $(top_srcdir)/src/include
CXXFLAGS+= -I$(SRCHEADERDIR) -I$(LIBPQDIR)

OBJS = pgconnection.o pgdatabase.o pgtransdb.o pgcursordb.o pglobject.o 

ifeq ($(PORTNAME), win)
SHLIB_LINK+= --driver-name g++ $(LIBPQ)
else
SHLIB_LINK= $(LIBPQ)
endif

# For CC on IRIX, must use CC as linker/archiver of C++ libraries
ifeq ($(PORTNAME), irix5)
  ifeq ($(CXX), CC)
    AR		:= CC
    AROPT	:= -ar -o
    LD		:= CC
  endif
endif
# Same for Solaris with native compiler
ifeq ($(PORTNAME), solaris_sparc)
  ifeq ($(CXX), CC)
    AR		:= CC
    AROPT	:= -xar -o
    LD		:= CC
  endif
endif
ifeq ($(PORTNAME), solaris_i386)
  ifeq ($(CXX), CC)
    AR		:= CC
    AROPT	:= -xar -o
    LD		:= CC
  endif
endif

all: all-lib

# Shared library stuff
include $(top_builddir)/src/Makefile.shlib

# Pull shared-lib CFLAGS into CXXFLAGS
CXXFLAGS+= $(CFLAGS_SL)


.PHONY: examples
examples:
	$(MAKE) -C examples all

install: all installdirs install-headers install-lib

libpqxx_includedir = $(includedir)/libpq++
MAINHEADER = libpq++.h
LIBPGXXHEADERS = pgconnection.h pgdatabase.h pgtransdb.h pgcursordb.h pglobject.h

.PHONY: install-headers
install-headers: $(MAINHEADER) $(LIBPGXXHEADERS)
	$(INSTALL_DATA) $< $(includedir)
	for i in $(LIBPGXXHEADERS); do $(INSTALL_DATA) $(srcdir)/$$i $(libpqxx_includedir) || exit 1; done

installdirs:
	$(mkinstalldirs) $(libdir) $(includedir) $(libpqxx_includedir)

uninstall: uninstall-lib
	rm -f $(includedir)/$(MAINHEADER)
	rm -rf $(libpqxx_includedir)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	-$(MAKE) -C examples clean


dep depend:
	$(CXX) -MM $(CXXFLAGS) *.cc >depend

ifeq (depend,$(wildcard depend))
include depend
endif
