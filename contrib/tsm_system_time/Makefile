# contrib/tsm_system_time/Makefile

MODULE_big = tsm_system_time
OBJS = \
	$(WIN32RES) \
	tsm_system_time.o
PGFILEDESC = "tsm_system_time - TABLESAMPLE method which accepts time in milliseconds as a limit"

EXTENSION = tsm_system_time
DATA = tsm_system_time--1.0.sql

REGRESS = tsm_system_time

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tsm_system_time
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
