# contrib/pg_upgrade/Makefile

PGFILEDESC = "pg_upgrade - an in-place binary upgrade utility"
PGAPPICON = win32

PROGRAM  = pg_upgrade
OBJS = check.o controldata.o dump.o exec.o file.o function.o info.o \
       option.o page.o parallel.o pg_upgrade.o relfilenode.o server.o \
       tablespace.o util.o version.o version_old_8_3.o $(WIN32RES)

PG_CPPFLAGS  = -DFRONTEND -DDLSUFFIX=\"$(DLSUFFIX)\" -I$(srcdir) -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

EXTRA_CLEAN = analyze_new_cluster.sh delete_old_cluster.sh log/ tmp_check/ \
              pg_upgrade_dump_globals.sql \
              pg_upgrade_dump_*.custom pg_upgrade_*.log

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_upgrade
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

check: test.sh all
	MAKE=$(MAKE) bindir=$(bindir) libdir=$(libdir) EXTRA_REGRESS_OPTS="$(EXTRA_REGRESS_OPTS)" $(SHELL) $< --install

# disabled because it upsets the build farm
#installcheck: test.sh
#	MAKE=$(MAKE) bindir=$(bindir) libdir=$(libdir) $(SHELL) $<
