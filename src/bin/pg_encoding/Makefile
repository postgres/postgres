#-------------------------------------------------------------------------
#
#    Makefile for bin/pg_encoding
#
# Copyright (c) 1998, PostgreSQL development group
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/pg_encoding/Attic/Makefile,v 1.4 2000/01/15 18:30:32 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

# This is a bit of hackery here: pg_encoding uses backend includes and the
# frontend library.

OBJS= pg_encoding.o
CFLAGS:= -I$(SRCDIR)/include $(CFLAGS)

all: submake pg_encoding

pg_encoding: $(OBJS)
	$(CC) -o pg_encoding $(OBJS) -L$(LIBPQDIR) -lpq $(LDFLAGS) $(CFLAGS)

.PHONY: submake

submake:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install: pg_encoding
	$(INSTALL) $(INSTL_EXE_OPTS) pg_encoding$(X) $(BINDIR)/pg_encoding$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f pg_encoding pg_encoding.o

ifeq (depend,$(wildcard depend))
include depend
endif
