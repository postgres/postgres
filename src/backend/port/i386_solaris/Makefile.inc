#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/sparc_solaris (SPARC/Solaris 2.x specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/i386_solaris/Attic/Makefile.inc,v 1.1 1996/07/20 08:34:32 scrappy Exp $
#
#-------------------------------------------------------------------------

CFLAGS+= -DUSE_POSIX_TIME -DNEED_ISINF -DNEED_RUSAGE -DNO_EMPTY_STMTS

LDADD+= -ll -ldl

SUBSRCS+= port.c

HEADERS+= machine.h port-protos.h rusagestub.h
