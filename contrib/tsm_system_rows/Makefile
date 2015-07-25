# contrib/tsm_system_rows/Makefile

MODULE_big = tsm_system_rows
OBJS = tsm_system_rows.o $(WIN32RES)
PGFILEDESC = "tsm_system_rows - TABLESAMPLE method which accepts number of rows as a limit"

EXTENSION = tsm_system_rows
DATA = tsm_system_rows--1.0.sql

REGRESS = tsm_system_rows

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tsm_system_rows
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
