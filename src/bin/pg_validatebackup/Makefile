# src/bin/pg_validatebackup/Makefile

PGFILEDESC = "pg_validatebackup - validate a backup against a backup manifest"
PGAPPICON = win32

subdir = src/bin/pg_validatebackup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# We need libpq only because fe_utils does.
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	parse_manifest.o \
	pg_validatebackup.o

all: pg_validatebackup

pg_validatebackup: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_validatebackup$(X) '$(DESTDIR)$(bindir)/pg_validatebackup$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_validatebackup$(X)'

clean distclean maintainer-clean:
	rm -f pg_validatebackup$(X) $(OBJS)

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
