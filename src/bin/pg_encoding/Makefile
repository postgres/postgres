#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_encoding
#
# Copyright (c) 1998, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_encoding/Makefile,v 1.16 2003/11/29 19:52:05 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_encoding
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= pg_encoding.o

all: submake-libpq submake-libpgport pg_encoding

pg_encoding: $(OBJS)
	$(CC) $(CFLAGS) $^ $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pg_encoding$(X) $(DESTDIR)$(bindir)/pg_encoding$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_encoding$(X)

clean distclean maintainer-clean:
	rm -f pg_encoding$(X) pg_encoding.o
