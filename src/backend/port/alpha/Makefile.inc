#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/alpha (Alpha OSF/1 specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/alpha/Attic/Makefile.inc,v 1.2 1996/10/18 04:47:04 scrappy Exp $
#
#-------------------------------------------------------------------------

LDADD+= -lln

#
# The YACC grammar is too big..
#
#.if !defined(CDEBUG)
##CFLAGS+= -Olimit 2000
#.endif

HEADERS+= machine.h port-protos.h

SUBSRCS= port.c
