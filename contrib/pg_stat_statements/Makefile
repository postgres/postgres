# contrib/pg_stat_statements/Makefile

MODULE_big = pg_stat_statements
OBJS = pg_stat_statements.o

EXTENSION = pg_stat_statements
DATA = pg_stat_statements--1.1.sql pg_stat_statements--1.0--1.1.sql \
	pg_stat_statements--unpackaged--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_statements
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
