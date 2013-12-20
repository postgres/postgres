# contrib/pg_prewarm/Makefile

MODULE_big = pg_prewarm
OBJS = pg_prewarm.o

EXTENSION = pg_prewarm
DATA = pg_prewarm--1.0.sql

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
