#-------------------------------------------------------------------------
#
# Makefile for src/bin (client programs)
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/Makefile,v 1.44 2004/06/18 21:24:05 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

DIRS := initdb ipcclean pg_ctl pg_dump \
	psql scripts pg_config pg_controldata pg_resetxlog

all install installdirs uninstall depend distprep:
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	-@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
