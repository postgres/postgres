# src/test/modules/test_custom_types/Makefile

MODULES = test_custom_types

EXTENSION = test_custom_types
DATA = test_custom_types--1.0.sql
PGFILEDESC = "test_custom_types - tests for dummy custom types"

REGRESS = test_custom_types

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_custom_types
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
