#-------------------------------------------------------------------------
#
# Makefile for src/pl/plpgsql (PostgreSQL's SQL procedural language)
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/pl/plpgsql/Makefile,v 1.6 2001/05/24 15:53:34 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/pl/plpgsql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifneq ($(PORTNAME), qnx4)

all install installdirs uninstall distprep:
	$(MAKE) -C src $@

else  #qnx4

all install installdirs uninstall distprep:
	echo plpgsql not supported with QNX due to lack of shared library support

endif # qnx4

clean distclean maintainer-clean:
	-$(MAKE) -C src $@
