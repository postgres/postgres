#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_id
#
# Copyright (C) 2000 by PostgreSQL Global Development Team
#
# $Header: /cvsroot/pgsql/src/bin/pg_id/Attic/Makefile,v 1.17 2000/06/27 00:31:20 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_id
top_builddir = ../../..
include ../../Makefile.global

all: pg_id$(X)

pg_id$(X): pg_id.o
	$(CC) -o $@ $^ $(LDFLAGS)

install: all installdirs
	$(INSTALL_PROGRAM) pg_id$(X) $(bindir)/pg_id$(X)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/pg_id$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_id$(X) pg_id.o

ifeq (depend,$(wildcard depend))
include depend
endif
