#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.47 2002/08/22 22:43:11 scrappy Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := libpq ecpg libpgeasy

ALLDIRS := $(DIRS) libpgtcl perl5 python jdbc

ifeq ($(with_tcl), yes)
DIRS += libpgtcl
endif

ifeq ($(with_perl), yes)
DIRS += perl5
endif

ifeq ($(with_python), yes)
DIRS += python
endif

ifeq ($(with_java), yes)
DIRS += jdbc
endif


all install installdirs uninstall dep depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

distclean maintainer-clean:
	@for dir in $(ALLDIRS); do $(MAKE) -C $$dir $@; done
