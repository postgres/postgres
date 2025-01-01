#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_controldata
#
# Copyright (c) 1998-2025, PostgreSQL Global Development Group
#
# src/bin/pg_controldata/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_controldata - reads the data from pg_control"
PGAPPICON=win32

subdir = src/bin/pg_controldata
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pg_controldata.o

all: pg_controldata

pg_controldata: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_controldata$(X) '$(DESTDIR)$(bindir)/pg_controldata$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_controldata$(X)'

clean distclean:
	rm -f pg_controldata$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
