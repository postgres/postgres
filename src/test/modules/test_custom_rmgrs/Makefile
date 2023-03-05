# src/test/modules/test_custom_rmgrs/Makefile

MODULE_big = test_custom_rmgrs
OBJS = \
	$(WIN32RES) \
	test_custom_rmgrs.o
PGFILEDESC = "test_custom_rmgrs - test custom WAL resource managers"

EXTENSION = test_custom_rmgrs
DATA = test_custom_rmgrs--1.0.sql

EXTRA_INSTALL = contrib/pg_walinspect
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_custom_rmgrs
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
