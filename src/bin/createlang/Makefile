#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createlang
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createlang/Attic/Makefile,v 1.1 1999/05/20 16:50:00 wieck Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: createlang

createlang: createlang.sh
	sed -e 's/__DLSUFFIX__/$(DLSUFFIX)/'			\
			createlang.sh > createlang

install: createlang
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f createlang

dep depend:
