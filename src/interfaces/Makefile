#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = libpq ecpg

ifeq ($(with_libcurl), yes)
SUBDIRS += libpq-oauth
else
ALWAYS_SUBDIRS += libpq-oauth
endif

$(recurse)
$(recurse_always)

all-ecpg-recurse: all-libpq-recurse
install-ecpg-recurse: install-libpq-recurse

ifeq ($(with_libcurl), yes)
all-libpq-oauth-recurse: all-libpq-recurse
install-libpq-oauth-recurse: install-libpq-recurse
endif
