#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_version
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_version/Attic/Makefile,v 1.1.1.1 1996/07/09 06:22:14 scrappy Exp $
#
#-------------------------------------------------------------------------

PROG=	pg_version

MKDIR=	../../mk
include $(MKDIR)/postgres.mk
include ../Makefile.global

VPATH:=$(VPATH):$(srcdir)/backend/utils/init
SRCS=	pg_version.c magic.c

CFLAGS+= -I$(srcdir)/backend/port/$(PORTNAME)

include $(MKDIR)/postgres.prog.mk

