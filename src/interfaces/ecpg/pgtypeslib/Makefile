#-------------------------------------------------------------------------
#
# Makefile for ecpg pgtypes library
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/ecpg/pgtypeslib/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/pgtypeslib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

PGFILEDESC = "pgtypes - library for data type mapping"
NAME= pgtypes
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 7

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-DFRONTEND $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

# Need to recompile any libpgport object files
LIBS := $(filter-out -lpgport, $(LIBS))

SHLIB_LINK += -lm

SHLIB_EXPORTS = exports.txt

OBJS= numeric.o datetime.o common.o dt_common.o timestamp.o interval.o \
	pgstrcasecmp.o \
	$(filter rint.o snprintf.o, $(LIBOBJS)) $(WIN32RES)

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# We use some port modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.

pgstrcasecmp.c rint.c snprintf.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS) pgstrcasecmp.c rint.c snprintf.c

maintainer-clean: distclean maintainer-clean-lib
