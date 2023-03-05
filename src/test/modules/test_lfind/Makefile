# src/test/modules/test_lfind/Makefile

MODULE_big = test_lfind
OBJS = \
	$(WIN32RES) \
	test_lfind.o
PGFILEDESC = "test_lfind - test code for optimized linear search functions"

EXTENSION = test_lfind
DATA = test_lfind--1.0.sql

REGRESS = test_lfind

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_lfind
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
