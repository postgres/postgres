# src/test/modules/test_predtest/Makefile

MODULE_big = test_predtest
OBJS = \
	$(WIN32RES) \
	test_predtest.o
PGFILEDESC = "test_predtest - test code for optimizer/util/predtest.c"

EXTENSION = test_predtest
DATA = test_predtest--1.0.sql

REGRESS = test_predtest

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_predtest
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
