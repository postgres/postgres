#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.10 1998/05/29 17:00:29 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include $(SRCDIR)/Makefile.global

.DEFAULT all:
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
	cd perl5 && perl Makefile.PL
	$(MAKE) -C perl5 $@
endif
