#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/ultrix (Ultrix4.x specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/ultrix4/Attic/Makefile.inc,v 1.2 1996/10/18 04:49:09 scrappy Exp $
#
#-------------------------------------------------------------------------

LDADD+= -ldl -lln

#
# The YACC grammar is too big..
#
#.if !defined(CDEBUG)
#CFLAGS+= -Olimit 2000
#.endif

HEADERS+= dl.h machine.h port-protos.h

SUBSRCS+= dynloader.c port.c strdup.c
