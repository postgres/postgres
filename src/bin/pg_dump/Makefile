#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.9 1996/11/26 03:19:36 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../Makefile.global
include ../../Makefile.global

CFLAGS+= -I../../backend/port/${PORTNAME}

OBJS= pg_dump.o common.o

all: submake pg_dump

pg_dump: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(LDFLAGS) -o pg_dump -L$(LIBPQDIR) $(OBJS) -lpq $(LD_ADD)

.PHONY: submake
submake:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install: pg_dump
	$(INSTALL) $(INSTL_EXE_OPTS) pg_dump $(DESTDIR)$(BINDIR)/pg_dump

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f pg_dump $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
