#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initlocation
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initlocation/Attic/Makefile,v 1.8 1999/12/08 10:29:43 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: initlocation

initlocation: initlocation.sh
	cp -p initlocation.sh initlocation

install: initlocation
	$(INSTALL) $(INSTL_EXE_OPTS) $+ $(BINDIR)

clean:
	rm -f initlocation

dep depend:
