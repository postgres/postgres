#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/irix5 (IRIX 5 specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    /usr/local/devel/pglite/cvs/src/backend/port/sparc_solaris/Makefile.inc,v 1.3 1995/03/21 06:51:21 andrew Exp
#
#-------------------------------------------------------------------------

CFLAGS+= -DUSE_POSIX_TIME -DNEED_ISINF -DNO_EMPTY_STMTS

LDADD+= -ll

SUBSRCS+= port.c

HEADERS+= machine.h port-protos.h 
