# src/test/modules/test_tidstore/Makefile

MODULE_big = test_tidstore
OBJS = \
	$(WIN32RES) \
	test_tidstore.o
PGFILEDESC = "test_tidstore - test code for src/backend/access/common/tidstore.c"

EXTENSION = test_tidstore
DATA = test_tidstore--1.0.sql

REGRESS = test_tidstore

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_tidstore
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
