#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/destroyuser
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/destroyuser/Attic/Makefile,v 1.2 1996/11/11 13:40:04 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

SEDSCRIPT= \
    -e "s^_fUnKy_BINDIR_sTuFf_^$(BINDIR)^g" \
    -e "s^_fUnKy_POSTPORT_sTuFf_^$(POSTPORT)^g" \
    -e "s^_fUnKy_DASH_N_sTuFf_^$(DASH_N)^g" \
    -e "s^_fUnKy_BACKSLASH_C_sTuFf_^$(BACKSLASH_C)^g"

all: destroyuser

destroyuser:
	sed $(SEDSCRIPT) <destroyuser.sh >destroyuser

install: destroyuser
	$(INSTALL) $(INSTL_EXE_OPTS) $< $(DESTDIR)$(BINDIR)/$<

clean:
	rm -f destroyuser
