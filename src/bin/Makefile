#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.16 1998/11/14 01:56:36 thomas Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

DIRS = pg_id pg_version psql pg_dump pg_passwd cleardbdir createdb \
	createuser destroydb destroyuser initdb vacuumdb initlocation ipcclean

ifdef MULTIBYTE
DIRS += pg_encoding
endif

#
# TCL/TK programs
#
ifeq ($(USE_TCL), true)
	DIRS += pgtclsh
endif

.DEFAULT all:
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
