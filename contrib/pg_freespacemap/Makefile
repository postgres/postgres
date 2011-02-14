# contrib/pg_freespacemap/Makefile

MODULE_big = pg_freespacemap
OBJS = pg_freespacemap.o

EXTENSION = pg_freespacemap
DATA = pg_freespacemap--1.0.sql pg_freespacemap--unpackaged--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_freespacemap
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
