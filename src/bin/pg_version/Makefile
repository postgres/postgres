#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_version
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_version/Attic/Makefile,v 1.2 1996/11/12 06:46:54 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../Makefile.global
include ../../Makefile.global

OBJS= pg_version.o ../../utils/version.o

all: pg_version

pg_version: submake $(OBJS)
	$(CC) $(LDFLAGS) -o pg_version $(OBJS) $(LD_ADD)

.PHONY: submake
submake:
	$(MAKE) -C ../../utils version.o

install: pg_version
	$(INSTALL) $(INSTL_EXE_OPTS) pg_version $(DESTDIR)$(BINDIR)/pg_version

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f pg_version pg_version.o

ifeq (depend,$(wildcard depend))
include depend
endif
