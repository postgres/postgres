# src/test/modules/test_toast_ext/Makefile

MODULE_big = test_toast_ext
OBJS = test_toast_ext.o

EXTENSION = test_toast_ext
DATA = test_toast_ext--1.0.sql

REGRESS = test_toast_ext

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_toast_ext
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
