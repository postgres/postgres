#-------------------------------------------------------------------------
#
# Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/pl/Makefile,v 1.19 2001/05/10 03:00:11 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := plpgsql

ifeq ($(with_tcl), yes)
DIRS += tcl
endif

ifeq ($(with_perl), yes)
DIRS += plperl
endif

#ifeq ($(with_python), yes)
#DIRS += plpython
#endif

all install installdirs uninstall depend distprep:
	@echo "PL/Python disabled until merged into our Makefile system, bjm 2001-05-09"
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
