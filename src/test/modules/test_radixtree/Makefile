# src/test/modules/test_radixtree/Makefile

MODULE_big = test_radixtree
OBJS = \
	$(WIN32RES) \
	test_radixtree.o
PGFILEDESC = "test_radixtree - test code for src/include/lib/radixtree.h"

EXTENSION = test_radixtree
DATA = test_radixtree--1.0.sql

REGRESS = test_radixtree

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_radixtree
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
