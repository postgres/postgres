#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.12 1998/08/17 03:53:37 scrappy Exp $
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
#	$(MAKE) -C ecpg $@
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

perl5/Makefile: perl5/Makefile.PL
	cd perl5 && perl Makefile.PL
