#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.16 1998/10/07 06:41:40 thomas Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include $(SRCDIR)/Makefile.global


perl-makefile-dep :=
ifeq ($(USE_PERL), true)
  perl-makefile-dep := perl5/Makefile
endif


.DEFAULT all install clean dep depend distclean: $(perl-makefile-dep)
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
	$(MAKE) -C perl5 $@
endif
ifeq ($(USE_ODBC), true)
	$(MAKE) -C odbc $@
endif

perl5/Makefile: perl5/Makefile.PL
	cd perl5 && perl Makefile.PL
