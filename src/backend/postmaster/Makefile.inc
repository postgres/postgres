#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the postmaster module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/postmaster/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:49 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/postmaster

SRCS_POSTMASTER= postmaster.c
