#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_id
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_id/Attic/Makefile,v 1.4 1997/02/06 02:31:25 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global
include ../Makefile.global

OBJS= pg_id.o

all: pg_id

pg_id: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(LDFLAGS) -o pg_id -L$(LIBPQDIR) $(OBJS) -lpq $(LD_ADD)

$(LIBPQDIR)/libpq.a:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install: pg_id
	$(INSTALL) $(INSTL_EXE_OPTS) pg_id $(DESTDIR)$(BINDIR)/pg_id

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f pg_id $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
