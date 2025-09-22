# src/test/modules/test_bitmapset/Makefile

MODULE_big = test_bitmapset
OBJS = \
	$(WIN32RES) \
	test_bitmapset.o
PGFILEDESC = "test_bitmapset - test code for bitmapset"

EXTENSION = test_bitmapset
DATA = test_bitmapset--1.0.sql

REGRESS = test_bitmapset

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_bitmapset
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
