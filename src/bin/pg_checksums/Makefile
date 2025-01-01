#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_checksums
#
# Copyright (c) 1998-2025, PostgreSQL Global Development Group
#
# src/bin/pg_checksums/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_checksums - verify data checksums in an offline cluster"
PGAPPICON=win32

subdir = src/bin/pg_checksums
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# We need libpq only because fe_utils does.
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	pg_checksums.o

all: pg_checksums

pg_checksums: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_checksums$(X) '$(DESTDIR)$(bindir)/pg_checksums$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_checksums$(X)'

clean distclean:
	rm -f pg_checksums$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
