#-------------------------------------------------------------------------
#
# Makefile for src/interfaces
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/Makefile,v 1.55 2007/02/09 15:55:59 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS = libpq ecpg 

ALLDIRS = $(DIRS) 

all install installdirs uninstall distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

distclean maintainer-clean:
	@for dir in $(ALLDIRS); do $(MAKE) -C $$dir $@; done
