# src/bin/pg_upgrade/Makefile

PGFILEDESC = "pg_upgrade - an in-place binary upgrade utility"
PGAPPICON = win32

subdir = src/bin/pg_upgrade
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = check.o controldata.o dump.o exec.o file.o function.o info.o \
       option.o parallel.o pg_upgrade.o relfilenode.o server.o \
       tablespace.o util.o version.o $(WIN32RES)

override CPPFLAGS := -DDLSUFFIX=\"$(DLSUFFIX)\" -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS += -L$(top_builddir)/src/fe_utils -lpgfeutils -lpq


all: pg_upgrade

pg_upgrade: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_upgrade$(X) '$(DESTDIR)$(bindir)/pg_upgrade$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_upgrade$(X)'

clean distclean maintainer-clean:
	rm -f pg_upgrade$(X) $(OBJS)
	rm -rf analyze_new_cluster.sh delete_old_cluster.sh log/ tmp_check/ \
	       pg_upgrade_dump_globals.sql \
	       pg_upgrade_dump_*.custom pg_upgrade_*.log

check: test.sh all
	MAKE=$(MAKE) bindir=$(bindir) libdir=$(libdir) EXTRA_REGRESS_OPTS="$(EXTRA_REGRESS_OPTS)" $(SHELL) $< --install

# disabled because it upsets the build farm
#installcheck: test.sh
#	MAKE=$(MAKE) bindir=$(bindir) libdir=$(libdir) $(SHELL) $<
