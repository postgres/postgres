# contrib/basic_wal_module/Makefile

MODULES = basic_wal_module
PGFILEDESC = "basic_wal_module - basic write-ahead log module"

REGRESS = basic_wal_module
REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/basic_wal_module/basic_wal_module.conf
# Disabled because these tests require "shared_preload_libraries=basic_wal_module",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/basic_wal_module
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
