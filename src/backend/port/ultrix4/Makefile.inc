#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/ultrix (Ultrix4.x specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/ultrix4/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:45 scrappy Exp $
#
#-------------------------------------------------------------------------

CFLAGS+= -DNEED_ISINF -DUSE_POSIX_TIME

LDADD+= -ldl -lln

#
# The YACC grammar is too big..
#
#.if !defined(CDEBUG)
#CFLAGS+= -Olimit 2000
#.endif

HEADERS+= dl.h machine.h port-protos.h

SUBSRCS+= dynloader.c port.c strdup.c
