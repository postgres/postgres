#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/pg_id
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_id/Attic/Makefile,v 1.9 1998/04/06 00:29:14 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

OBJS= pg_id.o

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LDFLAGS+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

all: pg_id

pg_id: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) -o pg_id -L$(LIBPQDIR) $(OBJS) -lpq $(LDFLAGS)

$(LIBPQDIR)/libpq.a:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install: pg_id
	$(INSTALL) $(INSTL_EXE_OPTS) pg_id $(BINDIR)/pg_id

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f pg_id $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
