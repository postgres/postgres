#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/linux (Linux specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/linux/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:44 scrappy Exp $
#
# NOTES
#    The Linux port is included here by courtesy of Kai Petzke.
#
#    (C) 1994, Kai Petzke, wpp@marie.physik.tu-berlin.de
#
#-------------------------------------------------------------------------

#
# linux has fast linkers and don't need the BIGOBJ stuff.
#
BIGOBJS= false


ifdef LINUX_ELF
CC=gcc
LDADD+= -ldl
CFLAGS+= -DLINUX_ELF
else
LDADD+= -ldld
SUBSRCS+= dynloader.c
endif

HEADERS+= machine.h port-protos.h
CFLAGS+= -DNEED_CBRT

