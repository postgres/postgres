#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_encoding
#
# Copyright (c) 1998, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/src/bin/pg_encoding/Attic/Makefile,v 1.9 2000/06/28 18:29:21 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_encoding
top_builddir = ../../..
include ../../Makefile.global

OBJS= pg_encoding.o

all: submake pg_encoding$(X)

pg_encoding$(X): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LIBPQ) $(LDFLAGS) $(CFLAGS)

.PHONY: submake

submake:
	$(MAKE) -C $(LIBPQDIR) all

install: all installdirs
	$(INSTALL_PROGRAM) pg_encoding$(X) $(bindir)/pg_encoding$(X)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/pg_encoding$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_encoding$(X) pg_encoding.o

ifeq (depend,$(wildcard depend))
include depend
endif
