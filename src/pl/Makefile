#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/pl/Makefile,v 1.3 2000/01/20 23:00:39 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include $(SRCDIR)/Makefile.global


.DEFAULT all install clean dep depend distclean:
	$(MAKE) -C plpgsql $@
ifeq ($(USE_TCL), true)
	$(MAKE) -C tcl $@
endif
ifeq ($(USE_PERL), true)
	-@if [ "$@" = "install" ]; then \
		$(MAKE) $(MFLAGS) install-plperl; \
	else \
		$(MAKE) $(MFLAGS) plperl/Makefile; \
		$(MAKE) $(MFLAGS) -C plperl $@; \
	fi
endif

plperl/Makefile: plperl/Makefile.PL
	cd plperl && $(PERL) Makefile.PL

install-plperl: plperl/Makefile
	$(MAKE) -C plperl clean
	cd plperl && POSTGRES_HOME="$(POSTGRESDIR)" $(PERL) Makefile.PL
	$(MAKE) -C plperl all
	-@if [ -w `sed -n -e 's/^ *INSTALLSITELIB *= *//p' plperl/Makefile` ]; then \
		$(MAKE) $(MFLAGS) -C plperl install; \
		rm -f plperl/Makefile; \
	else \
		echo "Skipping install of Perl module for lack of permissions."; \
		echo "To install it, cd into interfaces/plperl, su to become the"; \
		echo "appropriate user, and do '$(MAKE) install'."; \
	fi

.PHONY: install-plperl

