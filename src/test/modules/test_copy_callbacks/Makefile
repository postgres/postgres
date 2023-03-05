# src/test/modules/test_copy_callbacks/Makefile

MODULE_big = test_copy_callbacks
OBJS = \
	$(WIN32RES) \
	test_copy_callbacks.o
PGFILEDESC = "test_copy_callbacks - test COPY callbacks"

EXTENSION = test_copy_callbacks
DATA = test_copy_callbacks--1.0.sql

REGRESS = test_copy_callbacks

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_copy_callbacks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
