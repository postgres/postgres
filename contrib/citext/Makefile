# contrib/citext/Makefile

MODULES = citext

EXTENSION = citext
DATA = citext--1.0.sql citext--unpackaged--1.0.sql

REGRESS = citext

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/citext
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
