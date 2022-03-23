# src/test/modules/test_rls_hooks/Makefile

MODULE_big = test_rls_hooks
OBJS = \
	$(WIN32RES) \
	test_rls_hooks.o
PGFILEDESC = "test_rls_hooks - example use of RLS hooks"

REGRESS = test_rls_hooks

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_rls_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
