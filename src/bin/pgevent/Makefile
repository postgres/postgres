#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgevent
#
# Copyright (c) 1996-2024, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

PGFILEDESC = "Eventlog message formatter"
PGAPPICON=win32

subdir = src/bin/pgevent
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)

OBJS = \
	pgevent.o \
	pgmsgevent.o \
	$(WIN32RES)
NAME=pgevent

SHLIB_LINK =
SHLIB_EXPORTS = exports.txt

all: all-lib

install: all install-lib

include $(top_srcdir)/src/Makefile.shlib

pgmsgevent.o: pgmsgevent.rc
	$(WINDRES) $< -o $@ --include-dir=$(top_builddir)/src/include --include-dir=$(top_srcdir)/src/include --include-dir=$(srcdir) --include-dir=.

clean distclean: clean-lib
	rm -f $(OBJS) win32ver.rc

endif
