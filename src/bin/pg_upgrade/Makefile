# src/bin/pg_upgrade/Makefile

PGFILEDESC = "pg_upgrade - an in-place binary upgrade utility"
PGAPPICON = win32

# required for 003_upgrade_logical_replication_slots.pl
EXTRA_INSTALL=contrib/test_decoding

subdir = src/bin/pg_upgrade
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	check.o \
	controldata.o \
	dump.o \
	exec.o \
	file.o \
	function.o \
	info.o \
	option.o \
	parallel.o \
	pg_upgrade.o \
	relfilenumber.o \
	server.o \
	tablespace.o \
	task.o \
	util.o \
	version.o

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

# required for 002_pg_upgrade.pl
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

all: pg_upgrade

pg_upgrade: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_upgrade$(X) '$(DESTDIR)$(bindir)/pg_upgrade$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_upgrade$(X)'

clean distclean:
	rm -f pg_upgrade$(X) $(OBJS)
	rm -rf delete_old_cluster.sh log/ tmp_check/ \
	       reindex_hash.sql

export with_icu

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
