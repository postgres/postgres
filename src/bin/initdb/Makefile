#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.17 2000/02/16 21:25:59 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: initdb

initdb: initdb.sh
	sed -e 's/__MULTIBYTE__/$(MULTIBYTE)/g' < initdb.sh > initdb

install: initdb
	$(INSTALL) $(INSTL_EXE_OPTS) $+ $(BINDIR)/$+

clean:
	rm -f initdb

dep depend:
