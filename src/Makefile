#-------------------------------------------------------------------------
#
# Makefile for src
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/Makefile,v 1.28 2002/09/04 15:45:50 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src
top_builddir = ..
include Makefile.global


all install installdirs uninstall dep depend distprep:
	$(MAKE) -C port $@
	$(MAKE) -C backend $@
	$(MAKE) -C backend/utils/mb/conversion_procs $@
	$(MAKE) -C include $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@

install-all-headers:
	$(MAKE) -C include $@

clean:
	$(MAKE) -C port $@
	$(MAKE) -C backend $@
	$(MAKE) -C include $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@
	$(MAKE) -C test $@
	$(MAKE) -C tutorial $@

distclean maintainer-clean:
	-$(MAKE) -C port $@
	-$(MAKE) -C backend $@
	-$(MAKE) -C include $@
	-$(MAKE) -C interfaces $@
	-$(MAKE) -C bin $@
	-$(MAKE) -C pl $@
	-$(MAKE) -C test $@
	-$(MAKE) -C tutorial $@
	rm -f Makefile.port Makefile.global
