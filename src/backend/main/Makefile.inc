#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the main() of the postgres backend
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/main/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:31 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/main

SRCS_MAIN= main.c
