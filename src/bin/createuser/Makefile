#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createuser
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createuser/Attic/Makefile,v 1.9 1998/08/22 05:19:16 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^PG_OPT_DASH_N_PARAM^$(DASH_N)^g" \
    -e "s^PG_OPT_BACKSLASH_C_PARAM^$(BACKSLASH_C)^g"

all: createuser

createuser: createuser.sh
	sed $(SEDSCRIPT) <createuser.sh >createuser

install: createuser
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f createuser

dep depend:
