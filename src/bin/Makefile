#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.9 1997/08/25 19:59:52 momjian Exp $
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
	$(MAKE) -C pg_passwd $@
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
