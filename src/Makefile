#-------------------------------------------------------------------------
#
# Makefile for src
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/Makefile,v 1.32 2004/04/30 04:31:52 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src
top_builddir = ..
include Makefile.global


all install installdirs uninstall dep depend distprep:
	$(MAKE) -C port $@
	$(MAKE) -C timezone $@
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
	$(MAKE) -C timezone $@
	$(MAKE) -C backend $@
	$(MAKE) -C include $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@
	$(MAKE) -C test $@
	$(MAKE) -C tutorial $@
	$(MAKE) -C utils $@
	$(MAKE) -C tools/thread $@

distclean maintainer-clean:
	-$(MAKE) -C port $@
	-$(MAKE) -C timezone $@
	-$(MAKE) -C backend $@
	-$(MAKE) -C include $@
	-$(MAKE) -C interfaces $@
	-$(MAKE) -C bin $@
	-$(MAKE) -C pl $@
	-$(MAKE) -C test $@
	-$(MAKE) -C tutorial $@
	-$(MAKE) -C utils $@
	-$(MAKE) -C tools/thread $@
	rm -f Makefile.port Makefile.global
