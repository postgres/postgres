# contrib/pg_prewarm/Makefile

MODULE_big = pg_prewarm
OBJS = \
	$(WIN32RES) \
	autoprewarm.o \
	pg_prewarm.o

EXTENSION = pg_prewarm
DATA = pg_prewarm--1.1--1.2.sql pg_prewarm--1.1.sql pg_prewarm--1.0--1.1.sql
PGFILEDESC = "pg_prewarm - preload relation data into system buffer cache"

REGRESS = pg_prewarm

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_prewarm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
