#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for optimizer/util
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/util/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:38 scrappy Exp $
#
#-------------------------------------------------------------------------

SUBSRCS+= clauseinfo.c clauses.c indexnode.c internal.c plancat.c \
	joininfo.c keys.c ordering.c pathnode.c relnode.c tlist.c var.c
