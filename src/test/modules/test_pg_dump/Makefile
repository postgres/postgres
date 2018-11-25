# src/test/modules/test_pg_dump/Makefile

MODULE = test_pg_dump
PGFILEDESC = "test_pg_dump - Test pg_dump with an extension"

EXTENSION = test_pg_dump
DATA = test_pg_dump--1.0.sql

REGRESS = test_pg_dump
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_pg_dump
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
