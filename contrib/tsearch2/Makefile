# contrib/tsearch2/Makefile

MODULES = tsearch2

EXTENSION = tsearch2
DATA = tsearch2--1.0.sql tsearch2--unpackaged--1.0.sql

REGRESS = tsearch2

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
