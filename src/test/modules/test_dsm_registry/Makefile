# src/test/modules/test_dsm_registry/Makefile

MODULE_big = test_dsm_registry
OBJS = \
	$(WIN32RES) \
	test_dsm_registry.o
PGFILEDESC = "test_dsm_registry - test code for the DSM registry"

EXTENSION = test_dsm_registry
DATA = test_dsm_registry--1.0.sql

REGRESS = test_dsm_registry

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_dsm_registry
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
