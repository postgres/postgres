#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_resetxlog
#
# Copyright (c) 1998-2013, PostgreSQL Global Development Group
#
# src/bin/pg_resetxlog/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_resetxlog - reset PostgreSQL WAL log"
PGAPPICON=win32

subdir = src/bin/pg_resetxlog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= pg_resetxlog.o $(WIN32RES)

all: pg_resetxlog

pg_resetxlog: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_resetxlog$(X) '$(DESTDIR)$(bindir)/pg_resetxlog$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_resetxlog$(X)'

clean distclean maintainer-clean:
	rm -f pg_resetxlog$(X) $(OBJS)
