#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.12 1998/04/06 00:27:50 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

DIRS = pg_id pg_version psql pg_dump pg_passwd cleardbdir createuser \
	destroydb initdb initlocation

#
# TCL/TK programs
#
ifeq ($(USE_TCL), true)
	DIRS += pgtclsh
endif

.DEFAULT all:
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
