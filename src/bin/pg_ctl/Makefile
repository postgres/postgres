#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_ctl
#
# Copyright (c) 1999, PostgreSQL Global Development Group
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_ctl/Makefile,v 1.5 2000/06/04 01:44:36 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: pg_ctl

pg_ctl: pg_ctl.sh
	cat pg_ctl.sh > pg_ctl

install: pg_ctl
	$(INSTALL) $(INSTL_EXE_OPTS) $+ $(BINDIR)

clean:
	rm -f pg_ctl

dep depend:
