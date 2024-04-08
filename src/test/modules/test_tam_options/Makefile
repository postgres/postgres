# src/test/modules/test_tam_options/Makefile

MODULE_big = test_tam_options
OBJS = \
	$(WIN32RES) \
	test_tam_options.o
PGFILEDESC = "test_tam_options - test code for table access method reloptions"

EXTENSION = test_tam_options
DATA = test_tam_options--1.0.sql

REGRESS = test_tam_options

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_tam_options
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
