
#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for utils/adt
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:22:02 scrappy Exp $
#
#-------------------------------------------------------------------------

SUBSRCS+= acl.c arrayfuncs.c arrayutils.c bool.c char.c chunk.c date.c \
	datum.c dt.c filename.c float.c geo-ops.c geo-selfuncs.c int.c \
	misc.c nabstime.c name.c not_in.c numutils.c oid.c \
	oidname.c oidint2.c oidint4.c regexp.c regproc.c selfuncs.c \
	tid.c varchar.c varlena.c sets.c datetimes.c like.c

