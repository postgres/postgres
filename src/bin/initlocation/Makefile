#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/initlocation
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initlocation/Attic/Makefile,v 1.6 1998/08/22 05:19:25 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^PG_OPT_NAMEDATALEN_PARAM^$(NAMEDATALEN)^g" \
    -e "s^PG_OPT_OIDNAMELEN_PARAM^$(OIDNAMELEN)^g" 

all: initlocation

initlocation: initlocation.sh
	cp -p initlocation.sh initlocation

install: initlocation
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f initlocation

dep depend:
