#-------------------------------------------------------------------------
#
# Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
# src/pl/Makefile
#
#-------------------------------------------------------------------------

subdir = src/pl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = plpgsql

ifeq ($(with_perl), yes)
SUBDIRS += plperl
else
ALWAYS_SUBDIRS += plperl
endif

ifeq ($(with_python), yes)
SUBDIRS += plpython
else
ALWAYS_SUBDIRS += plpython
endif

ifeq ($(with_tcl), yes)
SUBDIRS += tcl
else
ALWAYS_SUBDIRS += tcl
endif

$(recurse)
$(recurse_always)
