#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_config
#
# Copyright (c) 1998-2024, PostgreSQL Global Development Group
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

all:
ifeq ($(PORTNAME), emscripten)
all: pg_config
else 
all: pg_config
endif

pg_config: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# the emscripten build generates multiple "runnable" files (.cjs/.mjs + .wasm)
# but other postgres tools expect exacly 'pg_config' as an executable 
# we create a pg_config link to run_pg_config.mjs, which in turn calls pg_config.cjs/.mjs
# pg_config.wasm is implicitly expected to be in the same folder

install: all installdirs
	$(INSTALL_SCRIPT) pg_config$(X) '$(DESTDIR)$(bindir)/pg_config$(X)'
ifeq ($(PORTNAME), emscripten)
	$(INSTALL_PROGRAM) pg_config.wasm '$(DESTDIR)$(bindir)/pg_config.wasm'
	$(INSTALL_PROGRAM) pg_config$(X) '$(DESTDIR)$(bindir)/pg_config.mjs'
	$(INSTALL_PROGRAM) run_pg_config.mjs '$(DESTDIR)$(bindir)/run_pg_config.mjs'
	ln -sf '$(DESTDIR)$(bindir)/run_pg_config.mjs' '$(DESTDIR)$(bindir)/pg_config'
endif

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_config$(X)'
ifeq ($(PORTNAME), emscripten)
	rm -f '$(DESTDIR)$(bindir)/pg_config.wasm'
	rm -f '$(DESTDIR)$(bindir)/pg_config'
endif


clean distclean:
	rm -f pg_config$(X) $(OBJS)
	rm -rf tmp_check
ifeq ($(PORTNAME), emscripten)
	rm -f pg_config.wasm
	rm -f pg_config
endif

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
