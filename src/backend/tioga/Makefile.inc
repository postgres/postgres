#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the Tioga module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tioga/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:22:00 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/tioga

SRCS_TIOGA= tgRecipe.c Varray.c

HEADERS+= tgRecipe.h Arr_TgRecipe.h Varray.h



