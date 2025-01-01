#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_walsummary
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_walsummary/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_walsummary - print contents of WAL summary files"
PGAPPICON=win32

subdir = src/bin/pg_walsummary
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils

OBJS = \
	$(WIN32RES) \
	pg_walsummary.o

all: pg_walsummary

pg_walsummary: $(OBJS) | submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)


install: all installdirs
	$(INSTALL_PROGRAM) pg_walsummary$(X) '$(DESTDIR)$(bindir)/pg_walsummary$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_walsummary$(X)'

clean distclean maintainer-clean:
	rm -f pg_walsummary$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
