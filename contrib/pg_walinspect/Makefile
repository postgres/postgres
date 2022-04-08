# contrib/pg_walinspect/Makefile

MODULE_big = pg_walinspect
OBJS = \
	$(WIN32RES) \
	pg_walinspect.o
PGFILEDESC = "pg_walinspect - functions to inspect contents of PostgreSQL Write-Ahead Log"

EXTENSION = pg_walinspect
DATA = pg_walinspect--1.0.sql

REGRESS = pg_walinspect

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_walinspect
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
