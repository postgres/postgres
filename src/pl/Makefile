#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/pl/Makefile,v 1.7 2000/04/11 17:42:28 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include $(SRCDIR)/Makefile.global


.DEFAULT all install clean dep depend distclean:
	$(MAKE) -C plpgsql $@
ifeq ($(USE_TCL), true)
	$(MAKE) -C tcl $@
endif
# does't work bjm 2000-04-11
#ifeq ($(USE_PERL), true)
#	-$(MAKE) $(MFLAGS) plperl/Makefile
#	-$(MAKE) $(MFLAGS) -C plperl $@
#endif

plperl/Makefile: plperl/Makefile.PL
	cd plperl && $(PERL) Makefile.PL POLLUTE=1
