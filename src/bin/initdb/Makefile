#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.15 1999/12/18 02:48:53 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: initdb

initdb: initdb.sh
	sed -e 's/__MULTIBYTE__/$(MULTIBYTE)/' initdb.sh > initdb

install: initdb
	$(INSTALL) $(INSTL_EXE_OPTS) $+ $(BINDIR)

clean:
	rm -f initdb

dep depend:
