#-------------------------------------------------------------------------
#
# Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/pl/Makefile,v 1.20 2001/05/12 01:30:30 petere Exp $
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

# Doesn't build portably yet.
#ifeq ($(with_python), yes)
#DIRS += plpython
#endif

all install installdirs uninstall depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
