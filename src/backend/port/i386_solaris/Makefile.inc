#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/i386_solaris (X86/Solaris 2.x specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/i386_solaris/Attic/Makefile.inc,v 1.5 1996/10/18 04:48:16 scrappy Exp $
#
#-------------------------------------------------------------------------

LDADD+= -ll -ldl

SUBSRCS+= port.c tas.s

HEADERS+= machine.h port-protos.h rusagestub.h
