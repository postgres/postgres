#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/Makefile,v 1.25 2004/07/13 00:06:41 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/ecpglib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= ecpg
SO_MAJOR_VERSION= 4
SO_MINOR_VERSION= 2
DLTYPE= library

override CPPFLAGS := -DFRONTEND -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_builddir)/src/port $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

OBJS= execute.o typename.o descriptor.o data.o error.o prepare.o memory.o \
	connect.o misc.o path.o exec.o

SHLIB_LINK = -L../pgtypeslib -lpgtypes $(libpq) \
	$(filter -lintl -lssl -lcrypto -lkrb5 -lcrypt -lm, $(LIBS)) $(PTHREAD_LIBS)

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# We use some port modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.

path.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

exec.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir)

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) path.c exec.c

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
