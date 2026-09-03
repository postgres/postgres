# src/test/modules/index/Makefile

MODULE_big = test_indexscan
OBJS = \
	$(WIN32RES) \
	test_indexscan.o
EXTENSION = test_indexscan
DATA = test_indexscan--1.0.sql
PGFILEDESC = "test_indexscan - test index scan internals"

EXTRA_INSTALL = contrib/btree_gin contrib/btree_gist

REGRESS = hot_chain

ISOLATION = killtuples

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/index
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
