# contrib/pg_freespacemap/Makefile

MODULE_big = pg_freespacemap
OBJS = pg_freespacemap.o $(WIN32RES)

EXTENSION = pg_freespacemap
DATA = pg_freespacemap--1.1.sql pg_freespacemap--1.0--1.1.sql \
	pg_freespacemap--unpackaged--1.0.sql
PGFILEDESC = "pg_freespacemap - monitoring of free space map"

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
