#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/createuser
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/createuser/Attic/Makefile,v 1.8 1998/04/06 16:49:37 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^_fUnKy_DASH_N_sTuFf_^$(DASH_N)^g" \
    -e "s^_fUnKy_BACKSLASH_C_sTuFf_^$(BACKSLASH_C)^g"

all: createuser

createuser: createuser.sh
	sed $(SEDSCRIPT) <createuser.sh >createuser

install: createuser
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(BINDIR)/$<

clean:
	rm -f createuser

dep depend:
