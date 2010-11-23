# contrib/pg_buffercache/Makefile

MODULE_big = pg_buffercache
OBJS = pg_buffercache_pages.o

DATA_built = pg_buffercache.sql
DATA = uninstall_pg_buffercache.sql

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
