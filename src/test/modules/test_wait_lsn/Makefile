# src/test/modules/test_wait_lsn/Makefile

MODULE_big = test_wait_lsn
OBJS = \
	$(WIN32RES) \
	test_wait_lsn.o
PGFILEDESC = "test_wait_lsn - test code for WAIT FOR LSN"

EXTENSION = test_wait_lsn
DATA = test_wait_lsn--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_wait_lsn
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
