#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.4 1996/07/31 17:35:13 scrappy Exp $
#
#-------------------------------------------------------------------------

PROG=	pg_dump

MKDIR=	../../mk
include $(MKDIR)/postgres.mk
include ../Makefile.global
CFLAGS += -I ../../backend/port/$(PORTNAME)
SRCS=	pg_dump.c common.c

include $(MKDIR)/postgres.prog.mk

