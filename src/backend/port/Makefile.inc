#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the port module (for code specific to various UNIX 
#    platforms)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:41 scrappy Exp $
#
#-------------------------------------------------------------------------

portdir= $(CURDIR)/port/$(PORTNAME)
VPATH:= $(VPATH):$(portdir)

SUBSRCS=
include $(portdir)/Makefile.inc
SRCS_PORT:= $(SUBSRCS)

