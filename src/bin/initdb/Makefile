#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.11 1998/07/26 04:31:16 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: initdb

initdb: initdb.sh
	sed 's/__MULTIBYTE__/$(MULTIBYTE)/' initdb.sh > initdb

install: initdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f initdb

dep depend:
