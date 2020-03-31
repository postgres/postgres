#-------------------------------------------------------------------------
#
# Makefile for ecpg pgtypes library
#
# Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
SO_MINOR_VERSION= $(MAJORVERSION)

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-DFRONTEND $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

SHLIB_LINK_INTERNAL = -lpgcommon_shlib -lpgport_shlib
SHLIB_LINK += $(filter -lintl -lm, $(LIBS))
SHLIB_PREREQS = submake-libpgport

SHLIB_EXPORTS = exports.txt

OBJS = \
	$(WIN32RES) \
	common.o \
	datetime.o \
	dt_common.o \
	interval.o \
	numeric.o \
	timestamp.o

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)

maintainer-clean: distclean
