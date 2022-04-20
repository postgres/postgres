#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_config
#
# Copyright (c) 1998-2022, PostgreSQL Global Development Group
#
# src/bin/pg_config/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_config - report configuration information"
PGAPPICON=win32

subdir = src/bin/pg_config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pg_config.o

all: pg_config

pg_config: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_SCRIPT) pg_config$(X) '$(DESTDIR)$(bindir)/pg_config$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_config$(X)'

clean distclean maintainer-clean:
	rm -f pg_config$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
