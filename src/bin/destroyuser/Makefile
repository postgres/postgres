#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroyuser
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroyuser/Attic/Makefile,v 1.9 1998/08/22 05:19:19 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^PG_OPT_DASH_N_PARAM^$(DASH_N)^g" \
    -e "s^PG_OPT_BACKSLASH_C_PARAM^$(BACKSLASH_C)^g"

all: destroyuser

destroyuser: destroyuser.sh
	sed $(SEDSCRIPT) <destroyuser.sh >destroyuser

install: destroyuser
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f destroyuser

dep depend:
