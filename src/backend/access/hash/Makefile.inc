#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for access/hash (hash access method)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/hash/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:10 scrappy Exp $
#
#-------------------------------------------------------------------------

SUBSRCS+= hash.c hashfunc.c hashinsert.c hashovfl.c hashpage.c hashscan.c \
	hashsearch.c hashstrat.c hashutil.c



