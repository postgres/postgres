#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/sparc_solaris (SPARC/Solaris 2.x specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/sparc_solaris/Attic/Makefile.inc,v 1.2 1996/10/18 04:48:47 scrappy Exp $
#
#-------------------------------------------------------------------------

LDADD+= -ll -ldl

SUBSRCS+= port.c tas.s

HEADERS+= machine.h port-protos.h rusagestub.h
