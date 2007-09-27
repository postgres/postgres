#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_resetxlog
#
# Copyright (c) 1998-2007, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_resetxlog/Makefile,v 1.18 2007/09/27 19:53:43 tgl Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_resetxlog - reset PostgreSQL WAL log"
subdir = src/bin/pg_resetxlog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= pg_resetxlog.o pg_crc.o $(WIN32RES)

all: submake-libpgport pg_resetxlog

pg_resetxlog: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_resetxlog$(X) '$(DESTDIR)$(bindir)/pg_resetxlog$(X)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_resetxlog$(X)'

clean distclean maintainer-clean:
	rm -f pg_resetxlog$(X) $(OBJS) pg_crc.c
