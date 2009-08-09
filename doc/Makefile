#----------------------------------------------------------------------------
#
# PostgreSQL documentation top-level makefile
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/doc/Makefile,v 1.39 2009/08/09 22:47:59 petere Exp $
#
#----------------------------------------------------------------------------

subdir = doc
top_builddir = ..
include $(top_builddir)/src/Makefile.global

all distprep html man install installdirs uninstall clean distclean maintainer-clean:
	$(MAKE) -C src $@
