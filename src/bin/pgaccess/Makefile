#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for pgaccess
#
# Copyright (c) 1994, Regents of the University of California
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pgaccess/Attic/Makefile,v 1.4 1999/10/30 16:39:48 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include $(SRCDIR)/Makefile.global

.PHONY: all clean
all clean: 

.PHONY: install
install: pgaccess.tcl
	$(INSTALL) $(INSTL_EXE_OPTS) pgaccess.tcl $(BINDIR)/pgaccess
