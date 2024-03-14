# contrib/pg_visibility/Makefile

MODULE_big = pg_visibility
OBJS = \
	$(WIN32RES) \
	pg_visibility.o

EXTENSION = pg_visibility
DATA = pg_visibility--1.1.sql pg_visibility--1.1--1.2.sql \
	pg_visibility--1.0--1.1.sql
PGFILEDESC = "pg_visibility - page visibility information"

REGRESS = pg_visibility
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_visibility
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
