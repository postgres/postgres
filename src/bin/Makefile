#-------------------------------------------------------------------------
#
# Makefile for src/bin (client programs)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.30 2000/08/26 13:08:15 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin
top_builddir = ../..
include ../Makefile.global

DIRS := initdb initlocation ipcclean pg_ctl pg_dump pg_id \
	pg_passwd psql scripts pg-config

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

all install installdirs uninstall depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	-@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
