#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the rewrite rules module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/rewrite/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:51 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/rewrite


SRCS_REWRITE= rewriteRemove.c rewriteDefine.c \
	 rewriteHandler.c rewriteManip.c rewriteSupport.c locks.c

HEADERS+= rewriteRemove.h rewriteDefine.h rewriteHandler.h \
	rewriteManip.h rewriteSupport.h locks.h prs2lock.h

