#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the traffic cop module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tcop/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:59 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/tcop

SRCS_TCOP= aclchk.c dest.c fastpath.c postgres.c pquery.c utility.c

HEADERS+= dest.h fastpath.h pquery.h tcopdebug.h tcopprot.h utility.h
