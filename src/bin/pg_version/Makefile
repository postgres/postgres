#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_version
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_version/Attic/Makefile,v 1.5 1997/04/26 05:05:43 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

OBJS= pg_version.o ../../utils/version.o

CFLAGS+= -I$(SRCDIR)/include

all: pg_version

pg_version: submake $(OBJS)
	$(CC) -o pg_version $(OBJS) $(LDFLAGS)

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
