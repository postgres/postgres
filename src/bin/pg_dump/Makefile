#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.6 1996/10/31 05:56:01 momjian Exp $
#
#-------------------------------------------------------------------------

PROG=	pg_dump

MKDIR=	../../mk
include $(MKDIR)/postgres.mk
include ../Makefile.global

CFLAGS+= -I../../backend/port/$(PORTNAME)

SRCS=	pg_dump.c common.c

include $(MKDIR)/postgres.prog.mk

