#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.50 2003/08/01 04:24:57 scrappy Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := libpq ecpg 

ALLDIRS := $(DIRS) libpgtcl jdbc

ifeq ($(with_tcl), yes)
DIRS += libpgtcl
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
