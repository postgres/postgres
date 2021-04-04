# src/test/modules/spgist_name_ops/Makefile

MODULE_big = spgist_name_ops
OBJS = \
	$(WIN32RES) \
	spgist_name_ops.o
PGFILEDESC = "spgist_name_ops - test opclass for SP-GiST"

EXTENSION = spgist_name_ops
DATA = spgist_name_ops--1.0.sql

REGRESS = spgist_name_ops

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/spgist_name_ops
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
