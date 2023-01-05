# src/test/modules/test_pg_db_role_setting/Makefile

MODULE_big = test_pg_db_role_setting
OBJS = \
	$(WIN32RES) \
	test_pg_db_role_setting.o
EXTENSION = test_pg_db_role_setting
DATA = test_pg_db_role_setting--1.0.sql

PGFILEDESC = "test_pg_db_role_setting - tests for default GUC values stored in pg_db_role_settings"

REGRESS = test_pg_db_role_setting

# disable installcheck for now
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_pg_db_role_setting
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
