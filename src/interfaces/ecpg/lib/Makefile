#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/ecpg/lib/Attic/Makefile,v 1.6 2000/06/28 18:29:39 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/lib
top_builddir = ../../../..
include ../../../Makefile.global

NAME= ecpg
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 1.1

CFLAGS+= -I$(top_srcdir)/src/interfaces/ecpg/include -I$(LIBPQDIR)


OBJS= execute.o typename.o descriptor.o data.o error.o prepare.o memory.o \
	connect.o misc.o

SHLIB_LINK= $(LIBPQ)

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) $(libdir)

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
