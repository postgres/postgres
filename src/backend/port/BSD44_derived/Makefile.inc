#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/BSD44_derived (for OSs derived from 4.4-lite BSD)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/BSD44_derived/Attic/Makefile.inc,v 1.3 1996/10/19 06:36:52 scrappy Exp $
#
#-------------------------------------------------------------------------

#
# 4.4-lite BSD-derived OSs require that the lex library be included,
# in case yywrap is defined
#
LDADD+= -ll

HAVE_Cplusplus= true

#
# 4.4-lite BSD-derived OSs have a little trouble with partially-implemented
# dynamic loading soutines. See the comments in port-protos.h.
#
SUBSRCS= dl.c

HEADERS+= float.h machine.h port-protos.h
