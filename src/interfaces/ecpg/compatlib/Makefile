#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/compatlib/Makefile,v 1.11.2.3 2004/03/14 12:18:35 meskes Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/compatlib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= ecpg_compat
SO_MAJOR_VERSION= 1
SO_MINOR_VERSION= 2

override CPPFLAGS := -I$(top_srcdir)/src/interfaces/ecpg/include -I$(libpq_srcdir) -I$(top_srcdir)/src/include/utils $(CPPFLAGS) $(THREAD_CPPFLAGS)
SHLIB_LINK = -L../ecpglib -lecpg -L../pgtypeslib -lpgtypes $(libpq) \
	$(filter -lintl -lssl -lcrypto -lkrb5 -lcrypt -lm, $(LIBS)) $(THREAD_LIBS)

OBJS= informix.o

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
