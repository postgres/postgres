#-------------------------------------------------------------------------
#
# Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/pl/Makefile,v 1.9 2000/06/27 00:31:48 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl
top_builddir = ../..
include ../Makefile.global

DIRS := plpgsql

ifeq ($(USE_TCL), true)
DIRS += tcl
endif

# Disabled because it doesn't work
#ifeq ($(with_perl), yes)
#	$(MAKE) -C plperl $@
#endif

ALLDIRS := plpgsql tcl plperl


all install installdirs uninstall depend:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit 1; done

clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

distclean maintainer-clean:
	@for dir in $(ALLDIRS); do $(MAKE) -C $$dir $@; done
