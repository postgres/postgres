#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.12 1997/01/16 15:28:21 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../Makefile.global
include ../../Makefile.global

OBJS= pg_dump.o common.o

ifeq ($(PORTNAME), ultrix4)
OBJS+= ../../utils/strdup.o
endif

all: submake pg_dump

pg_dump: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(LDFLAGS) -o pg_dump -L$(LIBPQDIR) $(OBJS) -lpq $(LD_ADD)

../../utils/strdup.o:
	$(MAKE) -C ../../utils strdup.o

.PHONY: submake
submake:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install: pg_dump
	$(INSTALL) $(INSTL_EXE_OPTS) pg_dump $(DESTDIR)$(BINDIR)/pg_dump
	$(INSTALL) $(INSTL_EXE_OPTS) pg_dumpall $(DESTDIR)$(BINDIR)/pg_dumpall

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f pg_dump $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
