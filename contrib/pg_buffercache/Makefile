# contrib/pg_buffercache/Makefile

MODULE_big = pg_buffercache
OBJS = pg_buffercache_pages.o $(WIN32RES)

EXTENSION = pg_buffercache
DATA = pg_buffercache--1.1.sql pg_buffercache--1.0--1.1.sql pg_buffercache--unpackaged--1.0.sql
PGFILEDESC = "pg_buffercache - monitoring of shared buffer cache in real-time"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_buffercache
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
