#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for optimizer/path
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:35 scrappy Exp $
#
#-------------------------------------------------------------------------

SUBSRCS= allpaths.c clausesel.c costsize.c hashutils.c indxpath.c \
	joinpath.c joinrels.c joinutils.c mergeutils.c orindxpath.c \
	prune.c

# not ready yet: predmig.c xfunc.c



