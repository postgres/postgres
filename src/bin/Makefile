#-------------------------------------------------------------------------
#
# Makefile for src/bin (client programs)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.33 2000/10/10 22:01:52 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := initdb initlocation ipcclean pg_ctl pg_dump pg_id \
	pg_passwd psql scripts pg_config

ifdef MULTIBYTE
DIRS += pg_encoding
endif

ifeq ($(with_tcl), yes)
ifeq ($(with_tk), yes)
	DIRS += pgaccess
endif
	DIRS += pgtclsh
endif

all install installdirs uninstall depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	-@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
