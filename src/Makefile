#-------------------------------------------------------------------------
#
# Makefile for src
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/Makefile,v 1.26 2002/07/27 20:10:04 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src
top_builddir = ..
include Makefile.global


all install installdirs uninstall dep depend distprep:
	$(MAKE) -C port $@
	$(MAKE) -C backend $@
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

distclean maintainer-clean:
	-$(MAKE) -C port $@
	-$(MAKE) -C backend $@
	-$(MAKE) -C include $@
	-$(MAKE) -C interfaces $@
	-$(MAKE) -C bin $@
	-$(MAKE) -C pl $@
	-$(MAKE) -C test $@
	rm -f Makefile.port Makefile.global
