#-------------------------------------------------------------------------
#
# Makefile for ecpg pgtypes library
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/pgtypeslib/Makefile,v 1.26 2005/03/14 17:27:50 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/pgtypeslib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

NAME= pgtypes
SO_MAJOR_VERSION= 2
SO_MINOR_VERSION= 1
DLTYPE= library

override CPPFLAGS := -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(top_srcdir)/src/include/utils -I$(libpq_srcdir) $(CPPFLAGS) \
	$(PTHREAD_CFLAGS) -DFRONTEND

# Need to recomple any libpgport object files
LIBS := $(patsubst -lpgport,, $(LIBS))

SHLIB_LINK += -lm

OBJS= numeric.o datetime.o common.o dt_common.o timestamp.o interval.o \
	pgstrcasecmp.o \
	$(filter rint.o, $(LIBOBJS))

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

rint.c: %.c : $(top_srcdir)/src/port/%.c
	rm -f $@ && $(LN_S) $< .

pgstrcasecmp.c: %.c : $(top_srcdir)/src/port/%.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs install-lib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir)

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) rint.c pgstrcasecmp.c

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
