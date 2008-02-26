#-------------------------------------------------------------------------
#
# Makefile for ecpg compatibility library
#
# Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/compatlib/Makefile,v 1.39 2008/02/26 06:41:23 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/compatlib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= ecpg_compat
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 1
DLTYPE= library

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_srcdir)/src/include/utils $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

SHLIB_LINK = -L../ecpglib -lecpg -L../pgtypeslib -lpgtypes $(libpq) -lm \
	$(PTHREAD_LIBS)

SHLIB_EXPORTS = exports.txt

# Need to recompile any libpgport object files
LIBS := $(filter-out -lpgport, $(LIBS))

OBJS= informix.o $(filter snprintf.o, $(LIBOBJS))

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

snprintf.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(libdir)'

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS) snprintf.c

maintainer-clean: distclean maintainer-clean-lib
