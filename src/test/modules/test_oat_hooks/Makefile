# src/test/modules/test_oat_hooks/Makefile

MODULE_big = test_oat_hooks
OBJS = \
	$(WIN32RES) \
	test_oat_hooks.o
PGFILEDESC = "test_oat_hooks - example use of object access hooks"

REGRESS = test_oat_hooks
REGRESS_OPTS = --temp-config=$(top_srcdir)/src/test/modules/test_oat_hooks/test_oat_hooks.conf
# Disabled because these tests require "shared_preload_libraries=test_oat_hooks",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_oat_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
