#-------------------------------------------------------------------------
#
# Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/pl/Makefile,v 1.26 2007/06/01 19:38:07 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS = plpgsql

ifeq ($(with_perl), yes)
DIRS += plperl
endif

ifeq ($(with_python), yes)
DIRS += plpython
endif

ifeq ($(with_tcl), yes)
DIRS += tcl
endif

all install installdirs uninstall distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit 1; done

clean distclean maintainer-clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

# We'd like check operations to run all the subtests before failing.
check installcheck:
	@CHECKERR=0; for dir in $(DIRS); do \
		$(MAKE) -C $$dir $@ || CHECKERR=$$?; \
	done; \
	exit $$CHECKERR
