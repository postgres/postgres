#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/Makefile,v 1.1 1997/08/16 20:56:28 scrappy Exp $
#
#-------------------------------------------------------------------------

#include ../Makefile.global

.DEFAULT all:
	$(MAKE) -C libpq $@

ifeq ($(HAVE_Cplusplus), true)
	$(MAKE) -C libpq++ $@
endif

ifeq ($(USE_TCL), true)
	$(MAKE) -C libpgtcl $@
endif

#	$(MAKE) -C perl5 $@
