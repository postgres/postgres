#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/sparc_solaris (SPARC/Solaris 2.x specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/sparc_solaris/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:45 scrappy Exp $
#
#-------------------------------------------------------------------------

CFLAGS+= -DUSE_POSIX_TIME -DNEED_ISINF -DNEED_RUSAGE -DNO_EMPTY_STMTS

LDADD+= -ll -ldl

SUBSRCS+= port.c tas.s

HEADERS+= machine.h port-protos.h rusagestub.h
