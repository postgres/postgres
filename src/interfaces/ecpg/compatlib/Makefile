#-------------------------------------------------------------------------
#
# Makefile for ecpg compatibility library
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/ecpg/compatlib/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces/ecpg/compatlib
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

PGFILEDESC = "ECPG compat - compatibility library for ECPG"
NAME= ecpg_compat
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= $(MAJORVERSION)

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

SHLIB_LINK_INTERNAL = -L../ecpglib -lecpg -L../pgtypeslib -lpgtypes $(libpq_pgport_shlib)
SHLIB_LINK = $(filter -lintl -lm, $(LIBS)) $(PTHREAD_LIBS)
SHLIB_PREREQS = submake-ecpglib submake-pgtypeslib

SHLIB_EXPORTS = exports.txt

OBJS = \
	$(WIN32RES) \
	informix.o

PKG_CONFIG_REQUIRES_PRIVATE = libecpg, libpgtypes

all: all-lib

.PHONY: submake-ecpglib submake-pgtypeslib

submake-ecpglib:
	$(MAKE) -C $(top_builddir)/src/interfaces/ecpg/ecpglib all

submake-pgtypeslib:
	$(MAKE) -C $(top_builddir)/src/interfaces/ecpg/pgtypeslib all

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)
