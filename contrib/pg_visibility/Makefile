# contrib/pg_visibility/Makefile

MODULE_big = pg_visibility
OBJS = pg_visibility.o $(WIN32RES)

EXTENSION = pg_visibility
DATA = pg_visibility--1.0.sql
PGFILEDESC = "pg_visibility - page visibility information"

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
