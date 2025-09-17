#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_amcheck
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_amcheck/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_amcheck - detect corruption within database relations"
PGAPPICON=win32

EXTRA_INSTALL=contrib/amcheck contrib/pageinspect

subdir = src/bin/pg_amcheck
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	pg_amcheck.o

all: pg_amcheck

pg_amcheck: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)


install: all installdirs
	$(INSTALL_PROGRAM) pg_amcheck$(X) '$(DESTDIR)$(bindir)/pg_amcheck$(X)'
ifeq ($(PORTNAME), emscripten)
	$(INSTALL_PROGRAM) pg_amcheck.wasm '$(DESTDIR)$(bindir)/pg_amcheck.wasm'
endif

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_amcheck$(X)'
ifeq ($(PORTNAME), emscripten)
	rm -f '$(DESTDIR)$(bindir)/pg_amcheck.wasm'
endif

clean distclean:
	rm -f pg_amcheck$(X) $(OBJS)
	rm -rf tmp_check
ifeq ($(PORTNAME), emscripten)
	rm -f pg_amcheck.wasm
endif

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
