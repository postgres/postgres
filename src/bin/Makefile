#-------------------------------------------------------------------------
#
# Makefile for src/bin (client programs)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.27 2000/07/01 15:02:19 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin
top_builddir = ../..
include ../Makefile.global

DIRS := initdb initlocation ipcclean pg_ctl pg_dump pg_id \
	pg_passwd pg_version psql scripts

ifdef MULTIBYTE
DIRS += pg_encoding
endif

# TCL/TK programs
ifeq ($(USE_TCL), true)
ifeq ($(USE_TK), true)
	DIRS += pgaccess
endif
	DIRS += pgtclsh
endif

all install installdirs uninstall depend:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit 1; done

clean distclean maintainer-clean:
	-@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
