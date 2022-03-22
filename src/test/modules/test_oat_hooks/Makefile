# src/test/modules/test_oat_hooks/Makefile

MODULE_big = test_oat_hooks
OBJS = test_oat_hooks.o $(WIN32RES)
PGFILEDESC = "test_oat_hooks - example use of object access hooks"

REGRESS = test_oat_hooks

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
