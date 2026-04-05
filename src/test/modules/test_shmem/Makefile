# src/test/modules/test_shmem/Makefile

PGFILEDESC = "test_shmem - test code for shmem allocations"

MODULE_big = test_shmem
OBJS = \
	$(WIN32RES) \
	test_shmem.o

EXTENSION = test_shmem
DATA = test_shmem--1.0.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_shmem
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
