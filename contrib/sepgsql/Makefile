# contrib/sepgsql/Makefile

MODULE_big = sepgsql
OBJS = \
	$(WIN32RES) \
	database.o \
	dml.o \
	hooks.o \
	label.o \
	proc.o \
	relation.o \
	schema.o \
	selinux.o \
	uavc.o
DATA_built = sepgsql.sql
PGFILEDESC = "sepgsql - SELinux integration"

TAP_TESTS = 1

# Note: because we don't tell the Makefile there are any regression tests,
# we have to clean those result files explicitly
EXTRA_CLEAN = -r $(pg_regress_clean_files) tmp/ *.pp sepgsql-regtest.if sepgsql-regtest.fc

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/sepgsql
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += -lselinux
