# src/test/modules/test_custom_stats/Makefile

MODULES = test_custom_var_stats test_custom_fixed_stats

EXTENSION = test_custom_var_stats test_custom_fixed_stats

OBJS = \
	$(WIN32RES) \
	test_custom_var_stats.o \
	test_custom_fixed_stats.o
PGFILEDESC = "test_custom_stats - custom pgstats"

DATA = test_custom_var_stats--1.0.sql \
       test_custom_fixed_stats--1.0.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_custom_stats
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
