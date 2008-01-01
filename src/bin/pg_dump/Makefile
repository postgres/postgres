#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump
#
# Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/pg_dump/Makefile,v 1.65 2008/01/01 19:45:55 momjian Exp $
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

EXTRA_OBJS = $(top_builddir)/src/backend/parser/keywords.o

all: submake-libpq submake-libpgport submake-backend pg_dump pg_restore pg_dumpall

pg_dump: pg_dump.o common.o pg_dump_sort.o $(OBJS) $(libpq_builddir)/libpq.a 
	$(CC) $(CFLAGS) pg_dump.o common.o pg_dump_sort.o $(OBJS) $(EXTRA_OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

pg_restore: pg_restore.o $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) pg_restore.o $(OBJS) $(EXTRA_OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

pg_dumpall: pg_dumpall.o dumputils.o $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) pg_dumpall.o dumputils.o $(EXTRA_OBJS) $(WIN32RES) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

.PHONY: submake-backend
submake-backend:
	$(MAKE) -C $(top_builddir)/src/backend/parser keywords.o


install: all installdirs
	$(INSTALL_PROGRAM) pg_dump$(X) '$(DESTDIR)$(bindir)'/pg_dump$(X)
	$(INSTALL_PROGRAM) pg_restore$(X) '$(DESTDIR)$(bindir)'/pg_restore$(X)
	$(INSTALL_PROGRAM) pg_dumpall$(X) '$(DESTDIR)$(bindir)'/pg_dumpall$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, pg_dump$(X) pg_restore$(X) pg_dumpall$(X))

clean distclean maintainer-clean:
	rm -f pg_dump$(X) pg_restore$(X) pg_dumpall$(X) $(OBJS) pg_dump.o common.o pg_dump_sort.o pg_restore.o pg_dumpall.o
