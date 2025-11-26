# contrib/pg_datemath/Makefile

MODULES = pg_datemath

EXTENSION = pg_datemath
DATA = pg_datemath--1.0.sql
PGFILEDESC = "pg_datemath - Enhanced date difference functions"

REGRESS = pg_datemath

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_datemath
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

