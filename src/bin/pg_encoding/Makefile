#-------------------------------------------------------------------------
#
#    Makefile for bin/pg_encoding
#
# Copyright (c) 1998, PostgreSQL development group
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_encoding/Attic/Makefile,v 1.2 1998/07/26 04:31:18 scrappy Exp $
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
	$(INSTALL) $(INSTL_EXE_OPTS) pg_encoding $(BINDIR)/pg_encoding

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f pg_encoding pg_encoding.o

ifeq (depend,$(wildcard depend))
include depend
endif
