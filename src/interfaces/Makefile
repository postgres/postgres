#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.41 2000/07/19 16:30:06 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces
top_builddir = ../..
include ../Makefile.global

DIRS := libpq ecpg libpgeasy

ALLDIRS := $(DIRS) odbc libpq++ libpgtcl perl5 python

ifeq ($(enable_odbc), yes)
DIRS += odbc
endif

ifeq ($(with_CXX), yes)
DIRS += libpq++
endif

ifeq ($(USE_TCL), true)
DIRS += libpgtcl
endif

ifeq ($(with_perl), yes)
DIRS += perl5
endif

ifeq ($(with_python), yes)
DIRS += python
endif


all install installdirs uninstall dep depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

distclean maintainer-clean: clean
	@for dir in $(ALLDIRS); do $(MAKE) -C $$dir $@; done
