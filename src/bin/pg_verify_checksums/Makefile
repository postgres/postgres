#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_verify_checksums
#
# Copyright (c) 1998-2018, PostgreSQL Global Development Group
#
# src/bin/pg_verify_checksums/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_verify_checksums - verify data checksums in an offline cluster"
PGAPPICON=win32

subdir = src/bin/pg_verify_checksums
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= pg_verify_checksums.o $(WIN32RES)

all: pg_verify_checksums

pg_verify_checksums: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_verify_checksums$(X) '$(DESTDIR)$(bindir)/pg_verify_checksums$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_verify_checksums$(X)'

clean distclean maintainer-clean:
	rm -f pg_verify_checksums$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
