# src/test/modules/test_aio/Makefile

PGFILEDESC = "test_aio - test code for AIO"

MODULE_big = test_aio
OBJS = \
	$(WIN32RES) \
	test_aio.o

EXTENSION = test_aio
DATA = test_aio--1.0.sql

TAP_TESTS = 1

export enable_injection_points

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_aio
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
