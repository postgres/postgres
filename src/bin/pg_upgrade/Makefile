# src/bin/pg_upgrade/Makefile

PGFILEDESC = "pg_upgrade - an in-place binary upgrade utility"
PGAPPICON = win32

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
	relfilenode.o \
	server.o \
	tablespace.o \
	util.o \
	version.o

override CPPFLAGS := -DDLSUFFIX=\"$(DLSUFFIX)\" -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

all: pg_upgrade

pg_upgrade: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_upgrade$(X) '$(DESTDIR)$(bindir)/pg_upgrade$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_upgrade$(X)'

clean distclean maintainer-clean:
	rm -f pg_upgrade$(X) $(OBJS)
	rm -rf delete_old_cluster.sh log/ tmp_check/ \
	       loadable_libraries.txt reindex_hash.sql \
	       pg_upgrade_dump_globals.sql \
	       pg_upgrade_dump_*.custom pg_upgrade_*.log

# When $(MAKE) is present, make automatically infers that this is a
# recursive make. which is not actually what we want here, as that
# e.g. prevents output synchronization from working (as make thinks
# that the subsidiary make knows how to deal with that itself, but
# we're invoking a shell script that doesn't know). Referencing
# $(MAKE) indirectly avoids that behaviour.
# See https://www.gnu.org/software/make/manual/html_node/MAKE-Variable.html#MAKE-Variable
NOTSUBMAKEMAKE=$(MAKE)

check: test.sh all temp-install
	MAKE=$(NOTSUBMAKEMAKE) $(with_temp_install) bindir=$(abs_top_builddir)/tmp_install/$(bindir) EXTRA_REGRESS_OPTS="$(EXTRA_REGRESS_OPTS)" $(SHELL) $<

# installcheck is not supported because there's no meaningful way to test
# pg_upgrade against a single already-running server
