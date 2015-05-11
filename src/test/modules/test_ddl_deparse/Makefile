MODULES = test_ddl_deparse
PGFILEDESC = "test_ddl_deparse - regression testing for DDL deparsing"

EXTENSION = test_ddl_deparse
DATA = test_ddl_deparse--1.0.sql

REGRESS = --schedule=$(srcdir)/regress_schedule
EXTRA_INSTALL = contrib/pg_stat_statements

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_ddl_deparse
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
