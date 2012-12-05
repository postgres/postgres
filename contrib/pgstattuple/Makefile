# contrib/pgstattuple/Makefile

MODULE_big	= pgstattuple
OBJS		= pgstattuple.o pgstatindex.o

EXTENSION = pgstattuple
DATA = pgstattuple--1.1.sql pgstattuple--1.0--1.1.sql pgstattuple--unpackaged--1.0.sql

REGRESS = pgstattuple

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgstattuple
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
