#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/alpha (Alpha OSF/1 specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/alpha/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:42 scrappy Exp $
#
#-------------------------------------------------------------------------

CFLAGS+= -DUSE_POSIX_TIME -DDISABLE_XOPEN_NLS -DNEED_ISINF -DHAS_LONG_LONG

LDADD+= -lln

#
# The YACC grammar is too big..
#
#.if !defined(CDEBUG)
##CFLAGS+= -Olimit 2000
#.endif

HEADERS+= machine.h port-protos.h

SUBSRCS= port.c
