#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initdb
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.12 1999/05/12 10:35:43 wieck Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: initdb

initdb: initdb.sh
	sed -e 's/__MULTIBYTE__/$(MULTIBYTE)/' 			\
		-e 's/__DLSUFFIX__/$(DLSUFFIX)/'			\
			initdb.sh > initdb

install: initdb
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f initdb

dep depend:
