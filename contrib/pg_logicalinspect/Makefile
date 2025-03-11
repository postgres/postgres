# contrib/pg_logicalinspect/Makefile

MODULE_big = pg_logicalinspect
OBJS = \
	$(WIN32RES) \
	pg_logicalinspect.o
PGFILEDESC = "pg_logicalinspect - functions to inspect logical decoding components"

EXTENSION = pg_logicalinspect
DATA = pg_logicalinspect--1.0.sql

EXTRA_INSTALL = contrib/test_decoding

REGRESS = pg_logicalinspect
ISOLATION = logical_inspect

ISOLATION_OPTS = --temp-config $(top_srcdir)/contrib/pg_logicalinspect/logicalinspect.conf

# Disabled because these tests require "wal_level=logical", which
# some installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_logicalinspect
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
