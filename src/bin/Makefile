#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.1.1.1 1996/07/09 06:22:11 scrappy Exp $
#
#-------------------------------------------------------------------------

#
# C programs
#
SUBDIR=  monitor pg_id pg_version psql pg_dump

ifeq ($(USE_TCL), true)
SUBDIR += pgtclsh
endif

#
# Shell scripts
#
SUBDIR+= cleardbdir createdb createuser destroydb destroyuser initdb 


include ../mk/postgres.subdir.mk

