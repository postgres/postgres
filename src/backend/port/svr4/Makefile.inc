#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/svr4 (Intel x86/Intel SVR4 specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    /usr/local/devel/pglite/cvs/src/backend/port/svr4/Makefile.inc,v 1.3 1995/03/21 06:51:21 andrew Exp
#
#-------------------------------------------------------------------------

LDADD+= -ll -ldl

SUBSRCS+= port.c

HEADERS+= machine.h port-protos.h rusagestub.h
