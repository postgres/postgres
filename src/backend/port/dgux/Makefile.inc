#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/linux (Linux specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/dgux/Attic/Makefile.inc,v 1.3 1996/10/18 04:47:13 scrappy Exp $
#
# NOTES
#    The Linux port is included here by courtesy of Kai Petzke.
#
#    (C) 1994, Kai Petzke, wpp@marie.physik.tu-berlin.de
#
#-------------------------------------------------------------------------

BIGOBJS= false

HEADERS+= machine.h port-protos.h
