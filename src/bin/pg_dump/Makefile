#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump
#
# Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/pg_dump/Makefile,v 1.67 2009/03/07 00:13:58 alvherre Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_dump/pg_restore/pg_dumpall - backup and restore PostgreSQL databases"
subdir = src/bin/pg_dump
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=	pg_backup_archiver.o pg_backup_db.o pg_backup_custom.o \
	pg_backup_files.o pg_backup_null.o pg_backup_tar.o \
	dumputils.o $(WIN32RES)

KEYWRDOBJS = keywords.o kwlookup.o

kwlookup.c: % : $(top_srcdir)/src/backend/parser/%
	rm -f $@ && $(LN_S) $< .

all: submake-libpq submake-libpgport pg_dump pg_restore pg_dumpall

pg_dump: pg_dump.o common.o pg_dump_sort.o $(OBJS) $(KEYWRDOBJS) $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) pg_dump.o common.o pg_dump_sort.o $(KEYWRDOBJS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

pg_restore: pg_restore.o $(OBJS) $(KEYWRDOBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) pg_restore.o $(KEYWRDOBJS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

pg_dumpall: pg_dumpall.o dumputils.o $(KEYWRDOBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) pg_dumpall.o dumputils.o $(KEYWRDOBJS) $(WIN32RES) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_dump$(X) '$(DESTDIR)$(bindir)'/pg_dump$(X)
	$(INSTALL_PROGRAM) pg_restore$(X) '$(DESTDIR)$(bindir)'/pg_restore$(X)
	$(INSTALL_PROGRAM) pg_dumpall$(X) '$(DESTDIR)$(bindir)'/pg_dumpall$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, pg_dump$(X) pg_restore$(X) pg_dumpall$(X))

clean distclean maintainer-clean:
	rm -f pg_dump$(X) pg_restore$(X) pg_dumpall$(X) $(OBJS) pg_dump.o common.o pg_dump_sort.o pg_restore.o pg_dumpall.o kwlookup.c $(KEYWRDOBJS)
