#-------------------------------------------------------------------------
#
#    Makefile for bin/pg_encoding
#
# Copyright (c) 1998, PostgreSQL development group
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_encoding/Attic/Makefile,v 1.3 1999/06/05 10:27:31 ishii Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

OBJS= pg_encoding.o

CFLAGS+= $(MBFLAGS) -I$(SRCDIR)/include

all: pg_encoding

pg_encoding: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) -o pg_encoding $(OBJS) -L$(LIBPQDIR) -lpq $(LDFLAGS)

install: pg_encoding
	$(INSTALL) $(INSTL_EXE_OPTS) pg_encoding$(X) $(BINDIR)/pg_encoding$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f pg_encoding pg_encoding.o

ifeq (depend,$(wildcard depend))
include depend
endif
