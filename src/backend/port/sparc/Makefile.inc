#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/sparc (SPARC/SunOS 4 specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/sparc/Attic/Makefile.inc,v 1.2 1996/10/18 04:48:32 scrappy Exp $
#
#-------------------------------------------------------------------------

LDADD+= -lln -ldl

#
# SunOS 4 strtol is broken -- doesn't report overflow using errno.
#
SUBSRCS= strtol.c

HEADERS+= float.h machine.h port-protos.h
