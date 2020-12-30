# contrib/old_snapshot/Makefile

MODULE_big = old_snapshot
OBJS = \
	$(WIN32RES) \
	time_mapping.o

EXTENSION = old_snapshot
DATA = old_snapshot--1.0.sql
PGFILEDESC = "old_snapshot - utilities in support of old_snapshot_threshold"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/old_snapshot
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
