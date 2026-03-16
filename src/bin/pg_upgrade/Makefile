# src/bin/pg_upgrade/Makefile

PGFILEDESC = "pg_upgrade - an in-place binary upgrade utility"
PGAPPICON = win32

EXTRA_INSTALL=contrib/test_decoding src/test/modules/dummy_seclabel src/test/modules/test_extensions

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
	multixact_read_v18.o \
	multixact_rewrite.o \
	option.o \
	parallel.o \
	pg_upgrade.o \
	relfilenumber.o \
	server.o \
	slru_io.o \
	tablespace.o \
	task.o \
	util.o \
	version.o

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

# required for 002_pg_upgrade.pl
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

# required for 008_extension_control_path.pl
TEST_EXT_LIB=$(abs_top_builddir)/src/test/modules/test_extensions/test_ext$(DLSUFFIX)
export TEST_EXT_LIB

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
