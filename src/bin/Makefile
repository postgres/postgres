#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.7 1997/04/26 05:04:21 scrappy Exp $
#
#-------------------------------------------------------------------------

#include ../Makefile.global

.DEFAULT all:
#
# C programs
#
	$(MAKE) -C pg_id $@
	$(MAKE) -C pg_version $@
	$(MAKE) -C psql $@
	$(MAKE) -C pg_dump $@
#
# Shell scripts
# 
	$(MAKE) -C cleardbdir $@
	$(MAKE) -C createdb $@
	$(MAKE) -C createuser $@
	$(MAKE) -C destroydb $@
	$(MAKE) -C destroyuser $@
	$(MAKE) -C initdb $@
#
# TCL/TK programs
#
ifeq ($(USE_TCL), true)
	$(MAKE) -C pgtclsh $@
endif
