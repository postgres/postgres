#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_ctl
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_ctl/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_ctl - starts/stops/restarts the PostgreSQL server"
PGAPPICON=win32

subdir = src/bin/pg_ctl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# On Windows, we need to link with libpq, just for use of pqexpbuffer;
# but let's not pull that in on platforms where we don't need it.
ifeq ($(PORTNAME), win32)
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)
SUBMAKE_LIBPQ := submake-libpq
endif

OBJS = \
	$(WIN32RES) \
	pg_ctl.o

all: pg_ctl

pg_ctl: $(OBJS) | submake-libpgport $(SUBMAKE_LIBPQ)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_ctl$(X) '$(DESTDIR)$(bindir)/pg_ctl$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_ctl$(X)'

clean distclean:
	rm -f pg_ctl$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
