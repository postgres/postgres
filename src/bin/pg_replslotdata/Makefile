#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_replslotdata
#
# Copyright (c) 1998-2021, PostgreSQL Global Development Group
#
# src/bin/pg_replslotdata/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_replslotdata - provides information about the replication slots from $PGDATA/pg_replslot/<slot_name> $PGDATA/pg_replslot/<slot_name>"
PGAPPICON=win32

subdir = src/bin/pg_replslotdata
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pg_replslotdata.o

all: pg_replslotdata

pg_replslotdata: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_replslotdata$(X) '$(DESTDIR)$(bindir)/pg_replslotdata$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_replslotdata$(X)'

clean distclean maintainer-clean:
	rm -f pg_replslotdata$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
