#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.23 2000/01/19 02:58:59 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

DIRS = pg_version psql pg_dump pg_passwd \
 	scripts initdb initlocation ipcclean \
	pg_ctl

ifdef MULTIBYTE
DIRS += pg_encoding
endif

#
# TCL/TK programs
#
ifeq ($(USE_TCL), true)
ifeq ($(USE_TK), true)
	DIRS += pgaccess
endif
	DIRS += pgtclsh
endif

.DEFAULT all:
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
