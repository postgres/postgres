#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/BSD44_derived (for OSs derived from 4.4-lite BSD)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/BSD44_derived/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:41 scrappy Exp $
#
#-------------------------------------------------------------------------

CFLAGS+= -DUSE_POSIX_TIME

#
# 4.4-lite BSD-derived OSs require that the lex library be included,
# in case yywrap is defined
#
LDADD+= -ll

#
# 4.4-lite BSD-derived OSs have a little trouble with partially-implemented
# dynamic loading soutines. See the comments in port-protos.h.
#
SUBSRCS= dl.c

HEADERS+= float.h machine.h port-protos.h
