#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_controldata
#
# Copyright (c) 1998, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/src/bin/pg_controldata/Makefile,v 1.1 2002/08/16 20:34:06 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_controldata
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= pg_controldata.o pg_crc.o

pg_crc.c: $(top_builddir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

all: submake-libpq submake-libpgport pg_controldata

pg_controldata: $(OBJS)
	$(CC) $(CFLAGS) $^ $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pg_controldata$(X) $(DESTDIR)$(bindir)/pg_controldata$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_controldata$(X)

clean distclean maintainer-clean:
	rm -f pg_controldata$(X) pg_controldata.o pg_crc.o pg_crc.c
