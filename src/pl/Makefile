#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/pl (procedural languages)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/pl/Makefile,v 1.1 1998/10/01 03:38:30 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include $(SRCDIR)/Makefile.global


.DEFAULT all install clean dep depend distclean:
	$(MAKE) -C plpgsql $@
ifeq ($(USE_TCL), true)
	$(MAKE) -C tcl $@
endif

