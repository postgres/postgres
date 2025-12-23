# src/test/modules/test_binaryheap/Makefile

MODULE_big = test_binaryheap
OBJS = \
	$(WIN32RES) \
	test_binaryheap.o

PGFILEDESC = "test_binaryheap - test code for binaryheap"

EXTENSION = test_binaryheap
DATA = test_binaryheap--1.0.sql

REGRESS = test_binaryheap

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_binaryheap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
