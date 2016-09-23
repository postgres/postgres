#-------------------------------------------------------------------------
#
# Makefile for src
#
# Copyright (c) 1994, Regents of the University of California
#
# src/Makefile
#
#-------------------------------------------------------------------------

subdir = src
top_builddir = ..
include Makefile.global

SUBDIRS = \
	common \
	port \
	timezone \
	backend \
	backend/utils/mb/conversion_procs \
	backend/snowball \
	include \
	interfaces \
	backend/replication/libpqwalreceiver \
	fe_utils \
	bin \
	pl \
	makefiles \
	test/regress \
	test/perl

# There are too many interdependencies between the subdirectories, so
# don't attempt parallel make here.
.NOTPARALLEL:

$(recurse)

install: install-local

install-local: installdirs-local
	$(INSTALL_DATA) Makefile.global '$(DESTDIR)$(pgxsdir)/$(subdir)/Makefile.global'
	$(INSTALL_DATA) Makefile.port '$(DESTDIR)$(pgxsdir)/$(subdir)/Makefile.port'
	$(INSTALL_DATA) $(srcdir)/Makefile.shlib '$(DESTDIR)$(pgxsdir)/$(subdir)/Makefile.shlib'
	$(INSTALL_DATA) $(srcdir)/nls-global.mk '$(DESTDIR)$(pgxsdir)/$(subdir)/nls-global.mk'

installdirs: installdirs-local

installdirs-local:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)'

uninstall: uninstall-local

uninstall-local:
	rm -f $(addprefix '$(DESTDIR)$(pgxsdir)/$(subdir)'/, Makefile.global Makefile.port Makefile.shlib nls-global.mk)

distprep:
	$(MAKE) -C test/isolation $@

clean:
	$(MAKE) -C test $@
	$(MAKE) -C tutorial NO_PGXS=1 $@
	$(MAKE) -C test/isolation $@
	$(MAKE) -C test/thread $@

distclean maintainer-clean:
	$(MAKE) -C test $@
	$(MAKE) -C tutorial NO_PGXS=1 $@
	$(MAKE) -C test/isolation $@
	$(MAKE) -C test/thread $@
	rm -f Makefile.port Makefile.global

coverage:
	$(MAKE) -C timezone $@
	$(MAKE) -C backend $@
	$(MAKE) -C backend/utils/mb/conversion_procs $@
	$(MAKE) -C backend/snowball $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C backend/replication/libpqwalreceiver $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@


.PHONY: install-local installdirs-local uninstall-local
