#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/bin (utility programs)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/Makefile,v 1.20 1999/12/05 20:02:43 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

DIRS = pg_id pg_version psql pg_dump pg_passwd cleardbdir \
 	scripts initdb initlocation ipcclean

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
