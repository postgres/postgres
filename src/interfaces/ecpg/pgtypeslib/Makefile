#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/ecpg/pgtypeslib/Makefile,v 1.1 2003/03/16 10:42:54 meskes Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/pgtypeslib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= pgtypes
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 0.0

override CPPFLAGS := -g -I$(top_srcdir)/src/interfaces/ecpg/include -I$(top_srcdir)/src/include/utils $(CPPFLAGS)

OBJS= numeric.o

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir)

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
