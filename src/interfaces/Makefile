#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.21 1998/12/18 17:53:22 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include $(SRCDIR)/Makefile.global


# Note: the klugery for perl5 is to ensure that the perl5 shared lib
# gets built with the correct path to the installed location of libpq
# during "make install", but is built against the local tree during
# ordinary building and testing.  During install, we must also guard
# against the likelihood that we don't have permissions to install into
# the Perl module library.

.DEFAULT all install clean dep depend distclean:
	$(MAKE) -C libpq $@
	$(MAKE) -C ecpg $@
ifeq ($(HAVE_Cplusplus), true)
	$(MAKE) -C libpq++ $@
else
	echo $(HAVE_Cplusplus): No C++
endif
ifeq ($(USE_TCL), true)
	$(MAKE) -C libpgtcl $@
endif
ifeq ($(USE_PERL), true)
	if [ "$@" = "install" ]; then \
		$(MAKE) $(MFLAGS) install-perl5; \
	else \
		$(MAKE) $(MFLAGS) perl5/Makefile; \
		$(MAKE) $(MFLAGS) -C perl5 $@; \
	fi
endif
ifeq ($(USE_ODBC), true)
	$(MAKE) -C odbc $@
endif

perl5/Makefile: perl5/Makefile.PL
	cd perl5 && perl Makefile.PL

install-perl5: perl5/Makefile
	$(MAKE) -C perl5 clean
	cd perl5 && POSTGRES_HOME="$(POSTGRESDIR)" perl Makefile.PL
	$(MAKE) -C perl5 all
	@if [ -w `sed -n -e 's/^ *SITELIBEXP *= *//p' perl5/Makefile` ]; then \
		$(MAKE) $(MFLAGS) -C perl5 install; \
		rm -f perl5/Makefile; \
	else \
		echo "Skipping install of Perl module for lack of permissions."; \
		echo "To install it, cd into interfaces/perl5, su to become the"; \
		echo "appropriate user, and do '$(MAKE) install'."; \
	fi

.PHONY: install-perl5
