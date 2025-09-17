# src/bin/pg_archivecleanup/Makefile

PGFILEDESC = "pg_archivecleanup - cleans archive when used with streaming replication"
PGAPPICON = win32

subdir = src/bin/pg_archivecleanup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pg_archivecleanup.o

all: pg_archivecleanup

pg_archivecleanup: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_archivecleanup$(X) '$(DESTDIR)$(bindir)/pg_archivecleanup$(X)'
ifeq ($(PORTNAME), emscripten)
	$(INSTALL_PROGRAM) pg_archivecleanup.wasm '$(DESTDIR)$(bindir)/pg_archivecleanup.wasm'
endif

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_archivecleanup$(X)'
ifeq ($(PORTNAME), emscripten)
	rm -f '$(DESTDIR)$(bindir)/pg_archivecleanup.wasm'
endif

clean distclean:
	rm -f pg_archivecleanup$(X) $(OBJS)
	rm -rf tmp_check
ifeq ($(PORTNAME), emscripten)
	rm -f pg_archivecleanup.wasm
endif	

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
