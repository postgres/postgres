# contrib/lo/Makefile

MODULES = lo

EXTENSION = lo
DATA = lo--1.1.sql lo--1.0--1.1.sql lo--unpackaged--1.0.sql
PGFILEDESC = "lo - management for large objects"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/lo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
