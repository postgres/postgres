#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.7 1996/11/11 13:40:46 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../Makefile.global
include ../../Makefile.global

OBJS= pg_dump.o common.o

all: submake pg_dump

pg_dump: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(LDFLAGS) -o pg_dump -L$(LIBPQDIR) $(OBJS) -lpq $(LD_ADD)

.PHONY: submake
submake:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install:
	$(INSTALL) $(INSTL_EXE_OPTS) pg_dump $(DESTDIR)$(BINDIR)/pg_dump

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f pg_dump $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
