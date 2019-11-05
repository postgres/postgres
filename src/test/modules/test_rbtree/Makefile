# src/test/modules/test_rbtree/Makefile

MODULE_big = test_rbtree
OBJS = \
	$(WIN32RES) \
	test_rbtree.o
PGFILEDESC = "test_rbtree - test code for red-black tree library"

EXTENSION = test_rbtree
DATA = test_rbtree--1.0.sql

REGRESS = test_rbtree

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_rbtree
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
