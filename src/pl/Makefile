#-------------------------------------------------------------------------
#
# Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/pl/Makefile,v 1.28 2009/08/07 20:50:22 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = plpgsql

ifeq ($(with_perl), yes)
SUBDIRS += plperl
endif

ifeq ($(with_python), yes)
SUBDIRS += plpython
endif

ifeq ($(with_tcl), yes)
SUBDIRS += tcl
endif

all install installdirs uninstall distprep clean distclean maintainer-clean:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done

# We'd like check operations to run all the subtests before failing.
check installcheck:
	@CHECKERR=0; for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $@ || CHECKERR=$$?; \
	done; \
	exit $$CHECKERR
