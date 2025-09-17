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
all: pg_config pg_config_sh
else 
all: pg_config
endif

pg_config: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# the emscripten build generates multiple "runnable" files (.cjs/.mjs + .wasm)
# but other postgres tools expect exacly 'pg_config' as an executable 
# the following rule creates a node script that calls the pg_config.cjs/.mjs
# pg_config.wasm is implicitly expected to be in the same folder
pg_config_sh: pg_config
	echo "#!/usr/bin/env node" > pg_config
	echo 'require("./pg_config$(X)")' >> pg_config
	chmod +x pg_config

install: all installdirs
	$(INSTALL_SCRIPT) pg_config$(X) '$(DESTDIR)$(bindir)/pg_config$(X)'
ifeq ($(PORTNAME), emscripten)
	$(INSTALL_PROGRAM) pg_config.wasm '$(DESTDIR)$(bindir)/pg_config.wasm'
	$(INSTALL_PROGRAM) pg_config '$(DESTDIR)$(bindir)/pg_config'
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
