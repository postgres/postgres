# contrib/pgstattuple/Makefile

MODULE_big	= pgstattuple
OBJS = \
	$(WIN32RES) \
	pgstatapprox.o \
	pgstatindex.o \
	pgstattuple.o

EXTENSION = pgstattuple
DATA = pgstattuple--1.4.sql pgstattuple--1.4--1.5.sql \
	pgstattuple--1.3--1.4.sql pgstattuple--1.2--1.3.sql \
	pgstattuple--1.1--1.2.sql pgstattuple--1.0--1.1.sql
PGFILEDESC = "pgstattuple - tuple-level statistics"

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
