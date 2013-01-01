#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_basebackup
#
# Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_basebackup/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_basebackup - takes a streaming base backup of a PostgreSQL instance"
PGAPPICON=win32

subdir = src/bin/pg_basebackup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=receivelog.o streamutil.o $(WIN32RES)

all: pg_basebackup pg_receivexlog

pg_basebackup: pg_basebackup.o $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) pg_basebackup.o $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

pg_receivexlog: pg_receivexlog.o $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) pg_receivexlog.o $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_basebackup$(X) '$(DESTDIR)$(bindir)/pg_basebackup$(X)'
	$(INSTALL_PROGRAM) pg_receivexlog$(X) '$(DESTDIR)$(bindir)/pg_receivexlog$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_basebackup$(X)'
	rm -f '$(DESTDIR)$(bindir)/pg_receivexlog$(X)'

clean distclean maintainer-clean:
	rm -f pg_basebackup$(X) pg_receivexlog$(X) $(OBJS) pg_basebackup.o pg_receivexlog.o
