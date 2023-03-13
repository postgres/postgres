# contrib/pg_walinspect/Makefile

MODULE_big = pg_walinspect
OBJS = \
	$(WIN32RES) \
	pg_walinspect.o
PGFILEDESC = "pg_walinspect - functions to inspect contents of PostgreSQL Write-Ahead Log"

EXTENSION = pg_walinspect
DATA = pg_walinspect--1.0.sql pg_walinspect--1.0--1.1.sql

REGRESS = pg_walinspect oldextversions

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/pg_walinspect/walinspect.conf

# Disabled because these tests require "wal_level=replica", which
# some installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

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
