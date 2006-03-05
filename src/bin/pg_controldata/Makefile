#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_controldata
#
# Copyright (c) 1998-2006, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_controldata/Makefile,v 1.14 2006/03/05 15:58:50 momjian Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_controldata - reads the data from pg_control"
subdir = src/bin/pg_controldata
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -DFRONTEND

OBJS= pg_controldata.o pg_crc.o $(WIN32RES)

all: submake-libpgport pg_controldata

pg_controldata: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_controldata$(X) '$(DESTDIR)$(bindir)/pg_controldata$(X)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_controldata$(X)'

clean distclean maintainer-clean:
	rm -f pg_controldata$(X) $(OBJS) pg_crc.c
