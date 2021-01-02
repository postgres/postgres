#-------------------------------------------------------------------------
#
# Makefile for ecpg library
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/ecpg/ecpglib/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/ecpglib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

PGFILEDESC = "ECPG - embedded SQL in C"
NAME= ecpg
SO_MAJOR_VERSION= 6
SO_MINOR_VERSION= $(MAJORVERSION)

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_builddir)/src/port -DFRONTEND $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

OBJS = \
	$(WIN32RES) \
	connect.o \
	data.o \
	descriptor.o \
	error.o \
	execute.o \
	memory.o \
	misc.o \
	prepare.o \
	sqlda.o \
	typename.o

SHLIB_LINK_INTERNAL = -L../pgtypeslib -lpgtypes $(libpq_pgport_shlib)
SHLIB_LINK = $(filter -lintl -lm, $(LIBS)) $(PTHREAD_LIBS)
SHLIB_PREREQS = submake-libpq submake-pgtypeslib

SHLIB_EXPORTS = exports.txt

PKG_CONFIG_REQUIRES_PRIVATE = libpq libpgtypes

all: all-lib

.PHONY: submake-pgtypeslib
submake-pgtypeslib:
	$(MAKE) -C $(top_builddir)/src/interfaces/ecpg/pgtypeslib all

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# Make dependency on pg_config_paths.h visible.
misc.o: misc.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)

maintainer-clean: distclean
