#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the nodes module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/nodes/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:32 scrappy Exp $
#
# NOTES
#    Originally, the nodes module is a home-brew, C++ like inheritance
#    system. However, the automatically generated tags, accessor functions
#    and the header files themselves are difficult to maintain. We need
#    real language support. Emulation doesn't quite do it...
#
#    See nodes/README for an explanation of the new no-frills nodes
#    structures.
#                                                     - ay 11/5/94
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/nodes


SRCS_NODES= nodeFuncs.c nodes.c list.c \
	copyfuncs.c equalfuncs.c makefuncs.c outfuncs.c readfuncs.c \
	print.c read.c

HEADERS+= execnodes.h makefuncs.h memnodes.h nodeFuncs.h nodes.h \
	params.h parsenodes.h pg_list.h plannodes.h primnodes.h relation.h

