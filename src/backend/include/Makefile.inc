#-------------------------------------------------------------------------
#
# Makefile.inc--
#    this makefile is only use for collecting HEADERS
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/include/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:28 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/include

HEADERS+= c.h libpq-fe.h miscadmin.h postgres.h
